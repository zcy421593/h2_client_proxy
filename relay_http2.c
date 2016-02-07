#include <stdio.h>
#include <string.h>
#include "ep.h"
#include "relay.h"
#include "nghttp2.h"
#include "header.h"
#include "list.h"
#include "conn.h"
#include "util.h"
#include "http_parser.h"

#include "ep_buffer.h"

static struct ep_base* s_base = NULL;
static nghttp2_session *s_session = NULL;
static bool s_tunnel_connecting = false;
static struct conn* s_conn_tunnel = NULL;
static int s_fd_tunnel = 0;
static list_head s_list_relays;
extern struct conn_proto tcp_conn_proto;
static struct conn_proto* s_conn_proto = &tcp_conn_proto;

struct http2_session {
  list_head list_relays;
};

struct http2_relay {
  list_head list;
  nghttp2_session* session;
  struct header* request_header;
  struct header* response_header;
  struct ep_timer* tmr_connectd;
  relay_eventcb cb;
  void* args;

  char* body_data;
  int body_len;
  
  int stream_id;

  bool has_length_info;
  bool is_request_complete;

  bool is_response_read_suspend;
  bool is_attached;

  nghttp2_data_provider req_provider;
  list_head list_write_buf;
  int relay_status;
  int local_window_size;
};

static void relay_close(void* relay);
static void relay_free(void* relay);

static void relay_check_length_info(struct http2_relay* relay) {
  
  const char* content_length = header_value(relay->response_header, "content-length");
  const char* chunked = header_value(relay->response_header, "Transfer-Encoding");
  const char* proxy_connection = header_value(relay->response_header, "proxy-connection");

  if(!proxy_connection) {
    if(relay->response_header->status_code < 500) {
      header_add_pair(relay->response_header, "proxy-connection", "keep-alive");
    } else {
      header_add_pair(relay->response_header, "proxy-connection", "close");
    }    
  }

  if(relay->response_header->status_code > 400) {
    return;
  }

  if(!content_length && !chunked) {
    relay->has_length_info = false;
  } else {
    relay->has_length_info = true;
  }

  if(!relay->has_length_info) {
    header_add_pair(relay->response_header, "Transfer-Encoding", "chunked");
  }
}

static ssize_t relay_session_send_callback(nghttp2_session *session , const uint8_t *data,
                             size_t length, int flags, void *user_data) {
  fprintf(stderr, "relay_session_send_callback\n");
  s_conn_proto->conn_write(s_conn_tunnel, (char*)data, length);
  return length;
}

static void relay_on_message_complete(struct http2_relay* relay) {
  fprintf(stderr, "relay_on_message_complete,streamid=%d\n", relay->stream_id);
  relay->relay_status = RELAY_STATUS_MSG_COMPLETE;
  relay->cb(relay, RELAY_MSG_COMPLETE, relay->args);
}

static void relay_on_response_header_complete(struct http2_relay* relay) {
  fprintf(stderr, "relay_on_response_header_complete\n");
  relay->relay_status = RELAY_STATUS_HEADER_COMPLETE;
  relay->cb(relay, RELAY_HEADER_COMPLETE, relay->args);
}

static int relay_session_on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data) {
  fprintf(stderr, "relay_session_on_frame_recv_callback:%d,flags=%d\n", frame->hd.type, frame->hd.flags);
  struct http2_relay *relay = (struct http2_relay *)nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);

  if(!relay || !relay->is_attached) {
    return 0;
  }

  switch (frame->hd.type) {
  case NGHTTP2_DATA: {
    if(frame->hd.flags && NGHTTP2_FLAG_END_STREAM) {
      relay_on_message_complete(relay);
    }
    break;
  }

  case NGHTTP2_HEADERS: {
    fprintf(stderr, "headers recv, count = %d\n", frame->headers.nvlen);
    bool is_end_stream = frame->hd.flags & NGHTTP2_FLAG_END_STREAM;
    bool is_end_header = frame->hd.flags & NGHTTP2_FLAG_END_HEADERS;

    if(is_end_header) {
      if(!is_end_stream && strcasecmp(relay->request_header->method, "connect") != 0) {
        relay_check_length_info(relay);
      }
      relay_on_response_header_complete(relay);

    }

    if (is_end_stream) {
      fprintf(stderr, "end stream found\n");
      relay_on_message_complete(relay);
    }
    break;
  }

  case NGHTTP2_RST_STREAM: {
    relay->cb(relay, RELAY_ERR_CONN_FAILED, relay->args);
    relay_free(relay);
    break;
  }

  case NGHTTP2_WINDOW_UPDATE: {
    fprintf(stderr, "recv window update package,len=%d\n", frame->window_update.window_size_increment);
    //nghttp2_session_get_stream_remote_window_size(nghttp2_session *session, int32_t stream_id)
    //frame->window_update.
  }

  default:
    break;
  }
  return 0;
}

static int relay_on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
                                    uint32_t error_code , void *user_data) {
  fprintf(stderr, "relay_on_stream_close_callback,stream id=%d\n", stream_id);
  struct http2_relay *relay = (struct http2_relay *)nghttp2_session_get_stream_user_data(session, stream_id);

  if (!relay) {
    return 0;
  }

  relay_free(relay);
  return 0;
}

static int relay_on_data_chunk_recv_callback(nghttp2_session *session,
                                       uint8_t flags, int32_t stream_id,
                                       const uint8_t *data, size_t len,
                                       void *user_data) {
  int local_window_len = nghttp2_session_get_stream_effective_recv_data_length(session, stream_id);
  int session_window_len = nghttp2_session_get_effective_local_window_size(session);
  //nghttp2_session_get_effective_recv_data_length(nghttp2_session *session)
  bool need_send = false;
  //nghttp2_session_get_stream_effective_recv_data_length(nghttp2_session *session, int32_t stream_id)
  fprintf(stderr, "relay_on_data_chunk_recv_callback, stream id=%d, local window len= %d, session_window_len=%d\n", stream_id, local_window_len, session_window_len);
  struct http2_relay *relay = (struct http2_relay *)nghttp2_session_get_stream_user_data(session, stream_id);;
  if(!relay || !relay->is_attached) {
    return 0;
  }

  relay->body_data = (char*)data;
  relay->body_len = len;

  relay->cb(relay, RELAY_BODY, relay->args);


  if(!relay->is_response_read_suspend && local_window_len > 30000) {
    nghttp2_submit_window_update(session, NGHTTP2_FLAG_NONE, stream_id, local_window_len); 
    need_send = true;   
  }

  if(session_window_len > 30000) {
    nghttp2_submit_window_update(session, NGHTTP2_FLAG_NONE, 0, session_window_len);
    need_send = true;   
  }

  if(need_send) {
    nghttp2_session_send(session);
  }
  
  return 0;
}

static int relay_on_header_callback(nghttp2_session *session,
                              const nghttp2_frame *frame, const uint8_t *name,
                              size_t namelen, const uint8_t *value,
                              size_t valuelen, uint8_t flags ,
                              void *user_data ) {
  char sz_field[1024] = {};
  char sz_value[4096] = {};

  strncpy(sz_field, (char*)name, sizeof(sz_field));
  strncpy(sz_value, (char*)value, sizeof(sz_value));
  fprintf(stderr, "relay_on_header_callback,streamid=%d, %s:%s\n", frame->hd.stream_id, sz_field, sz_value);
  struct http2_relay *relay;
  if (frame->hd.type != NGHTTP2_HEADERS) {
    return 0;
  }
  if (frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
    return 0;
  }

  relay = (struct http2_relay *)nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);

  if(!relay) {
    fprintf(stderr, "error:relay not found\n");
    return 0;
  }

  if(strcasecmp(sz_field, ":status") == 0) {
    relay->response_header->status_code = atoi(sz_value);
  } else {
    fprintf(stderr, "adding pair\n");
    header_add_pair(relay->response_header, sz_field, sz_value);
  }
  return 0;
}

static int relay_on_begin_headers_callback(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     void *user_data) {
  fprintf(stderr, "relay_on_begin_headers_callback, streamid=%d\n", frame->hd.stream_id);
  struct http2_relay *relay;

  return 0;
}

static void relay_init_h2_session() {
  nghttp2_session_callbacks *callbacks = NULL;
  nghttp2_option* opt = NULL;

  nghttp2_option_new(&opt);

  nghttp2_option_set_no_auto_window_update(opt, 1);

  nghttp2_session_callbacks_new(&callbacks);

  nghttp2_session_callbacks_set_send_callback(callbacks, relay_session_send_callback);
  
  
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                       relay_session_on_frame_recv_callback);

  nghttp2_session_callbacks_set_on_stream_close_callback(
      callbacks, relay_on_stream_close_callback);

  nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                   relay_on_header_callback);

  nghttp2_session_callbacks_set_on_begin_headers_callback(
      callbacks, relay_on_begin_headers_callback);

  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, relay_on_data_chunk_recv_callback);

  //nghttp2_session_callbacks_set_data_source_read_length_callback(callbacks, relay_session_data_source_read_length_callback);

  nghttp2_session_client_new2(&s_session, callbacks, NULL, opt);
  
  nghttp2_session_callbacks_del(callbacks);

  //nghttp2_option_del(opt);

  nghttp2_settings_entry iv[1] = {
      {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
  int rv;

  /* client 24 bytes magic string will be sent by nghttp2 library */
  rv = nghttp2_submit_settings(s_session, NGHTTP2_FLAG_NONE, iv,
                               1);
  if (rv != 0) {
    fprintf(stderr, "Could not submit SETTINGS: %s", nghttp2_strerror(rv));
  }
  nghttp2_session_send(s_session);
}

static void drop_all_clients() {
  fprintf(stderr, "drop all clients\n");
  struct http2_relay* relay = NULL;
  struct http2_relay* tmp = NULL;
  list_for_each_entry_safe(relay, tmp, &s_list_relays, list) {
    if(relay->is_attached) {
      relay->cb(relay, RELAY_ERR_CONN_FAILED, relay->args);
    }
    relay_free(relay);
  }
}

static void relay_tunnel_eventcb(struct conn* conn, short what, void* args) {
  struct http2_relay* relay = (struct http2_relay*)args;
  fprintf(stderr, "relay_tunnel_eventcb:%d\n", what);
  if(what == CONN_WRITE && s_tunnel_connecting) {
    relay_init_h2_session();
    s_tunnel_connecting = true;
    s_conn_proto->conn_detect(conn, CONN_READ, -1);
    fprintf(stderr, "tunnel connected\n");
    return;
  } if(what == CONN_READ) {
    char data[4096] = {};
    int recv_len = 0;
    recv_len = s_conn_proto->conn_read(s_conn_tunnel, data, sizeof(data));
    fprintf(stderr, "tunnel recv:%d\n", recv_len);

    if(recv_len <= 0) {
      s_conn_proto->conn_detect(conn, 0, -1);
      s_conn_proto->conn_close(conn);
      drop_all_clients();
      s_session = NULL;
      return;
    }
    nghttp2_session_mem_recv(s_session, (uint8_t*)data, recv_len);
    nghttp2_session_send(s_session);
  } else if(what & CONN_ERROR) {
    fprintf(stderr, "relay_tunnel_eventcb\n");
    s_conn_proto->conn_detect(conn, 0, -1);
    s_conn_proto->conn_close(conn);
    drop_all_clients();
    s_session = NULL;
  }
}

ssize_t relay_data_source_read_callback(
  nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length,
  uint32_t *data_flags, nghttp2_data_source *source, void *user_data) {
  fprintf(stderr, "relay_data_source_read_callback\n");
  struct http2_relay* relay = (struct http2_relay*)source->ptr;
  struct ep_buffer* pos = NULL;
  struct ep_buffer* n = NULL;
  int total_cpy = 0;

  list_for_each_entry_safe(pos, n, &relay->list_write_buf, list) {
    int len_left = length - total_cpy;
    int len_avalible = pos->len_total - pos->len_sent;
    int len_cpy = len_left < len_avalible ? len_left : len_avalible;
    memcpy(buf + total_cpy, pos->buf + pos->len_sent, len_cpy);
    pos->len_sent += len_cpy;
    total_cpy += len_cpy;

    if(pos->len_sent == pos->len_total) {
      list_del(&pos->list);
      ep_buffer_free(pos);
    } else {
      break;
    }
  }

  if(total_cpy == 0) {
    if(relay->is_request_complete && list_empty(&relay->list_write_buf)) {
      (*data_flags) |= NGHTTP2_DATA_FLAG_EOF;
      return 0;
    } else {
      fprintf(stderr, " body sent differed\n");
      return NGHTTP2_ERR_DEFERRED;
    }
    
  } else {
    fprintf(stderr, "%d body sent\n", total_cpy);
    return total_cpy;
  }

}

static int relay_sys_init(struct ep_base* base) {
  fprintf(stderr, "init h2_relay\n");
  INIT_LIST_HEAD(&s_list_relays);
  s_base = base;
  s_conn_tunnel = s_conn_proto->conn_create(s_base, "127.0.0.1", 6666, relay_tunnel_eventcb, NULL);
  s_conn_proto->conn_detect(s_conn_tunnel, CONN_WRITE, 30 * 1000);
  s_tunnel_connecting = true;
  return 0;
}

static void relay_tmrcb(struct ep_timer* tmr, void* args) {
  struct http2_relay* relay = (struct http2_relay*)args;
  if(s_session) {
    relay->cb(relay, RELAY_CONNECTED, relay->args);
  } else {
    relay->cb(relay, RELAY_ERR_CONN_FAILED, relay->args);
  }
}

static void* relay_create(const char* host, int port, relay_eventcb cb, void* args) {
  struct http2_relay* relay = (struct http2_relay*)calloc(1, sizeof(struct http2_relay));
  relay->session = s_session;
  relay->args = args;
  relay->cb = cb;
  relay->response_header = header_new(HEADER_RESPONSE);
  relay->tmr_connectd = ep_timer_new(s_base, relay_tmrcb, relay);
  relay->req_provider.source.ptr = relay;
  relay->req_provider.read_callback = relay_data_source_read_callback;
  relay->local_window_size = NGHTTP2_INITIAL_WINDOW_SIZE;
  relay->is_attached = true;
  ep_timer_add(relay->tmr_connectd, 0);
  INIT_LIST_HEAD(&relay->list_write_buf);
  list_add_tail(&relay->list, &s_list_relays);
  relay->relay_status = RELAY_STATUS_CONNECTING;
  return relay;
}

static void relay_make_nv(nghttp2_nv* nv, const char* name, const char* value) {
  nv->name = (uint8_t*)name;
  nv->namelen = strlen(name);
  nv->value = (uint8_t*)value;
  nv->valuelen = strlen(value);
}

static void relay_send_request(void* relay, const struct header* header) {
  fprintf(stderr, "relay_send_request:%s\n", header->url);
  nghttp2_nv nvs[100]= {};
  char schma[24] = {};
  char path[1024 * 4] = {};
  char query[1024 * 4] = {};
  char flagment[512] = {};
  char authority[128] = {};
  char port[10] = {};

  struct http2_relay* h2_relay = (struct http2_relay*)relay;
  h2_relay->request_header = (struct header*)header;
  const char* upgrade = header_value((struct header*)header, "upgrade");

  int count_lv = 0;
  struct field* pos = NULL;
  int i = 0;
  struct http_parser_url url = {};
  bool is_connect = (strcasecmp(header->method, "connect") == 0);

  http_parser_parse_url_(header->url, strlen(header->url), is_connect, &url);

  if(url.field_set & (1 << UF_SCHEMA)) {
    strncpy(schma, header->url + url.field_data[UF_SCHEMA].off, url.field_data[UF_SCHEMA].len);
    fprintf(stderr, "scheme:%s\n", schma);
  }

  if(url.field_set & (1 << UF_PATH)) {
    strncpy(path, header->url + url.field_data[UF_PATH].off, url.field_data[UF_PATH].len);
    fprintf(stderr, "path:%s\n", path);
  }

  if(!path[0]) {
    strcat(path, "/");
  }  

  if(url.field_set & (1 << UF_QUERY)) {
    strncpy(query, header->url + url.field_data[UF_QUERY].off, url.field_data[UF_QUERY].len);
    append_format(path,  sizeof(path), "?%s", query);
    fprintf(stderr, "query:%s\n", query);
  }

  if(url.field_set & (1 << UF_FRAGMENT)) {
    strncpy(flagment, header->url + url.field_data[UF_FRAGMENT].off, url.field_data[UF_FRAGMENT].len);
    append_format(path,  sizeof(path), "#%s", flagment);
    fprintf(stderr, "flagment:%s\n", flagment);
  }

  if(url.field_set & (1 << UF_HOST)) {
    strncpy(authority, header->url + url.field_data[UF_HOST].off, url.field_data[UF_HOST].len);
    fprintf(stderr, "authority:%s\n", authority);
  }  

  if(url.field_set & (1 << UF_PORT)) {
    strncpy(port, header->url + url.field_data[UF_PORT].off, url.field_data[UF_PORT].len);
  } else {
    strcpy(port, "80");
  }

  if(authority[0]) {
    strcat(authority, ":");
    strcat(authority, port);
  } else {
    const char* host = header_value((struct header*)header, "host");
    if(host) {
      strcpy(authority, host);
    }

  }
  
  if(!schma[0]) {
    strcpy(schma, "http");
  }

  fprintf(stderr, "method:%s\n", header->method);

  relay_make_nv(nvs + (i++), ":method", header->method);

  if(strcasecmp(header->method, "connect") != 0) {
    relay_make_nv(nvs + (i++), ":path", path);
    relay_make_nv(nvs + (i++), ":scheme", schma);
  }
  

  relay_make_nv(nvs + (i++), ":authority", authority);

  list_for_each_entry(pos, &header->list_headers, list) {    

    if(strcasecmp(pos->field, "Host") == 0) {
      continue;
    }

    if(strcasecmp(pos->field, "proxy-connection") == 0) {
      continue;
    }

    if(strcasecmp(pos->field, "connection") == 0) {
      continue;
    }

    if(strcasecmp(pos->field, "keep-alive") == 0) {
      continue;
    }

    fprintf(stderr, "%s:%s\n",pos->field, pos->value);
    relay_make_nv(nvs + i, pos->field, pos->value);
    i ++;
  }

  nghttp2_data_provider* provider = NULL;

  if(upgrade || strcasecmp(header->method, "post") == 0 || strcasecmp(header->method, "connect") == 0) {
    provider = &h2_relay->req_provider;
  }

  h2_relay->stream_id = nghttp2_submit_request(s_session, NULL, nvs, i, provider, relay);
  nghttp2_session_set_stream_user_data(s_session, h2_relay->stream_id, relay);
  fprintf(stderr, "request sent, id=%d\n", h2_relay->stream_id);
  nghttp2_session_send(s_session);
  h2_relay->relay_status = RELAY_STATUS_REQUEST_SENT;
}

static struct header* relay_get_header(void* relay) {
  struct http2_relay* h2_relay = (struct http2_relay*)relay;
  return h2_relay->response_header;
}

static int relay_get_body(void* relay, char** ptr_bldy, int* len_body) {
  struct http2_relay* h2_relay = (struct http2_relay*)relay;
  *ptr_bldy = h2_relay->body_data;
  *len_body = h2_relay->body_len;
  return 0;
}

static int relay_write_body(void* relay, char* data, int len) {
  fprintf(stderr, "relay_write_body,len=%d\n", len);
  struct http2_relay* h2_relay = (struct http2_relay*)relay;
  struct ep_buffer* write_buf = ep_buffer_create(data, len);
  list_add_tail(&write_buf->list, &h2_relay->list_write_buf);
  int window_size = nghttp2_session_get_stream_remote_window_size(h2_relay->session, h2_relay->stream_id);
  fprintf(stderr, "writting body, remote window =%d\n", window_size);

  if(h2_relay->relay_status >= RELAY_STATUS_REQUEST_SENT) {
    nghttp2_session_resume_data(h2_relay->session, h2_relay->stream_id);
    nghttp2_session_send(h2_relay->session);
  }
  return 0;
}

static void relay_close(void* relay) {
  struct http2_relay* h2_relay = (struct http2_relay*)relay;
  nghttp2_submit_rst_stream(h2_relay->session, 0, h2_relay->stream_id, 0);
  nghttp2_session_send(h2_relay->session);
  h2_relay->is_attached = false;
}

static void relay_free(void* relay) {
  struct http2_relay* h2_relay = (struct http2_relay*)relay;
  fprintf(stderr, "relay_close:%p, streamid=%d\n", relay, h2_relay->stream_id);
  nghttp2_session_set_stream_user_data(h2_relay->session, h2_relay->stream_id, NULL);
  list_del(&h2_relay->list);
  if(h2_relay->tmr_connectd) {
    ep_timer_del(h2_relay->tmr_connectd);
    ep_timer_free(h2_relay->tmr_connectd);
    h2_relay->tmr_connectd = NULL;
  }
  free(relay);
}

static int relay_get_status(void* relay) {
  struct http2_relay* h2_relay = (struct http2_relay*)relay;
  return h2_relay->relay_status;
}

static void relay_complete_request(void* relay) {
  struct http2_relay* h2_relay = (struct http2_relay*)relay;
  h2_relay->is_request_complete  = true;
}

static void relay_suspend_read(void* relay) {
  fprintf(stderr, "relay_suspend_read\n");
  struct http2_relay* h2_relay = (struct http2_relay*)relay;
  h2_relay->is_response_read_suspend = true;
}

static void relay_resume_read(void* relay) {
  fprintf(stderr, "relay_resume_read\n");
  struct http2_relay* h2_relay = (struct http2_relay*)relay;

  int local_window_len = nghttp2_session_get_stream_effective_recv_data_length(h2_relay->session, h2_relay->stream_id);

  if(h2_relay->is_response_read_suspend && local_window_len > 30000) {
    nghttp2_submit_window_update(h2_relay->session, 0, h2_relay->stream_id, local_window_len);
    nghttp2_session_send(h2_relay->session);
  }

  h2_relay->is_response_read_suspend = false;
}

struct relay_sys http2_relay_sys = {
  .relay_sys_init = relay_sys_init,
  .relay_create = relay_create,
  .relay_send_request = relay_send_request,
  .relay_get_header = relay_get_header,
  .relay_get_body = relay_get_body,
  .relay_get_status = relay_get_status,
  .relay_write_body = relay_write_body,
  .relay_close = relay_close,
  .relay_suspend_read = relay_suspend_read,
  .relay_resume_read = relay_resume_read,
  .relay_complete_request =relay_complete_request
};

