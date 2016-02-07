#include <stdio.h>
#include <stdlib.h>
#include "sys_network.h"
#include "client.h"
#include "ep.h"
#include "http_parser.h"
#include "list.h"
#include "header.h"
#include "relay.h"
#include "util.h"
#include "assert.h"
#include <errno.h>
#include "ep_buffer.h"
#define RECV_BUFFER_SIZE 4096

struct client_write_buffer {
  list_head list;
  void* buf;
  int len_total;
  int len_sent;
};

struct client {
  list_head list;
  int client_fd;  
  struct ep_file* file;

  bool relay_connected;
  struct relay_sys* relay_sys;
  void* relay;

  struct header* request_header;
  struct header* pending_request_header;
  struct header* response_header;

  http_parser parser;

  char* pending_response_body;
  int len_pending_response_body;
  list_head list_response_write_buffer;

  list_head list_request_body_buffer;
  int pending_write;
};

extern struct relay_sys http_relay_sys;
extern struct relay_sys http2_relay_sys;
static struct ep_base* s_base = NULL;
static list_head s_list_clients = {};

void client_drop(struct client* client);
static void client_relay_cb(void* relay, short what, void* args);

static int client_write(struct client* client, char* data, int len) {
  if(list_empty(&client->list_response_write_buffer)) {
    int len_write = send(client->client_fd, data, len, 0);
    fprintf(stderr, "client writting,%d=>%d\n", len, len_write);
    if(len_write < len) {
      if(len_write < 0) {
        if(errno != EINPROGRESS) {
          return -1;
        }
        len_write = 0;
      }
      int detect = ep_file_get_detect(client->file);
      int timeout = ep_file_get_timeout(client->file);
      struct client_write_buffer* buf = (struct client_write_buffer*)calloc(1, sizeof(struct client_write_buffer));
      buf->buf = malloc(len - len_write);
      buf->len_total = len - len_write;
      memcpy(buf->buf, data + len_write, len - len_write);
      list_add_tail(&buf->list, &client->list_response_write_buffer);
      client->pending_write += (len - len_write);
      detect |= EP_WRITE;
      ep_file_detect(client->file, detect, timeout);
    }
  } else {
    struct client_write_buffer* buf = (struct client_write_buffer*)calloc(1, sizeof(struct client_write_buffer));
    buf->buf = malloc(len);
    memcpy(buf->buf, data, len);
    buf->len_total = len;
    list_add_tail(&buf->list, &client->list_response_write_buffer);
    client->pending_write += len;
  }
  return 0;
}

static void client_handle_relay_connected(struct client* client) {
  assert(client->relay);
  fprintf(stderr, "%p:relay connected\n", client);
  struct ep_buffer* pos, *tmp;
  ep_file_detect(client->file, EP_READ, -1);
  client->relay_sys->relay_send_request(client->relay, client->request_header);
  list_for_each_entry_safe(pos, tmp, &client->list_request_body_buffer, list) {
    client->relay_sys->relay_write_body(client->relay, pos->buf, pos->len_total);
    list_del(&pos->list);
    ep_buffer_free(pos);
  }
}

static void client_handle_relay_hdr_complete(struct client* client) {
  struct header* resp_hdr = client->relay_sys->relay_get_header(client->relay);
  struct field* pos = NULL;
  char buf[50 * 4096] = {};
  const char* status_str = get_status_string(resp_hdr->status_code, 0);
  
  assert(status_str && status_str[0]);
  
  append_format(buf, sizeof(buf), "HTTP/1.1 %s\r\n", status_str);
  list_for_each_entry(pos, &resp_hdr->list_headers, list) {
    append_format(buf, sizeof(buf), "%s: %s\r\n", pos->field, pos->value);
  }
  strcat(buf, "\r\n");
  client_write(client, buf, strlen(buf));
  printf("client %p sending response:\n%s",client, buf);
}

static void client_handle_relay_body(struct client* client) {
  bool is_chunked = false;
  struct header* resp_hdr = client->relay_sys->relay_get_header(client->relay);
  const char* transfer_encoding = header_value(resp_hdr, "Transfer-Encoding");
  int len_body = 0;
  char* ptr_body = NULL;
  client->relay_sys->relay_get_body(client->relay, &ptr_body, &len_body);

  if(transfer_encoding && strcasecmp(transfer_encoding, "chunked") == 0) {
    is_chunked = true;
  }

  if(is_chunked) {
    char buf_chunk_len[64] = {};
    snprintf(buf_chunk_len, sizeof(buf_chunk_len), "%X\r\n", len_body);
    client_write(client, buf_chunk_len, strlen(buf_chunk_len));
  }
  fprintf(stderr, "client %p sending body:%d\n",client, len_body);
  client_write(client, ptr_body, len_body);

  if(is_chunked) {
    client_write(client, (char*)"\r\n", strlen("\r\n"));
  }

  fprintf(stderr, "client_handle_relay_body:after write, pending write body=%d\n", client->pending_write);

  if(client->pending_write > 30000) {
    fprintf(stderr, "pending write too large ,suspend read\n");
    client->relay_sys->relay_suspend_read(client->relay);
  }
}

static void client_handle_relay_msg_complete(struct client* client) {
  fprintf(stderr, "client %p msg completed\n", client);
  struct header* resp_hdr = client->relay_sys->relay_get_header(client->relay);
  const char* transfer_encoding = header_value(resp_hdr, "Transfer-Encoding");
  const char* content_length = header_value(resp_hdr, "Content-Length");
  if(transfer_encoding && strcasecmp(transfer_encoding, "chunked") == 0) {
    client_write(client, (char*)"0\r\n\r\n", strlen("0\r\n\r\n"));
  }

  client->relay = NULL;
  
  if(!transfer_encoding && !content_length) {
    client_drop(client);
    return;
  }
  

  if(client->request_header) {
    header_free(client->request_header);
    client->request_header = NULL;
  }
  
  if(client->pending_request_header) {
    client->request_header = client->pending_request_header;
     client->pending_request_header = NULL;

      
    char host[255] = {};
    int port = header_get_host_port(client->request_header);
    header_get_host(client->request_header, host, sizeof(host));

    client->relay = client->relay_sys->relay_create(host, port, client_relay_cb, client);
  }
}

static void client_relay_cb(void* relay, short what, void* args) {
  struct client* client = (struct client*)args;
  if(what == RELAY_CONNECTED) {
    client_handle_relay_connected(client);
  } else if(what == RELAY_HEADER_COMPLETE) {
    client_handle_relay_hdr_complete(client);
  } else if(what == RELAY_BODY) {
    client_handle_relay_body(client);
  } else if(what == RELAY_MSG_COMPLETE) {
    client_handle_relay_msg_complete(client);
  } else if(what == RELAY_ERR_CONN_FAILED) {
    fprintf(stderr, "client_relay_cb:conn err\n");
    client->relay = NULL;
    client_drop(client);
  } else if(what == RELAY_ERR_DNS_FAILED) {
    fprintf(stderr, "client_relay_cb:conn err\n");
    client->relay = NULL;
    client_drop(client);
  }
}

static int client_msg_begin(http_parser* parser) {
  fprintf(stderr, "client_msg_begin:%p\n", parser->data);
  struct client* client = (struct client*)parser->data;

  if(client->relay) {
    assert(client->request_header);
    client->pending_request_header = header_new(HEADER_REQUEST);;
  } else {
    assert(client->request_header == NULL);
    client->request_header = header_new(HEADER_REQUEST);
  }  
  return 0;
}

static int client_url_complete(http_parser* parser, const char *data, size_t len)
{

  struct client* client = (struct client*)parser->data;
  struct header* req_header = client->pending_request_header ? client->pending_request_header : 
                              client->request_header;
  header_set_url(req_header, data, len);
  fprintf(stderr, "client_url_complete:%s\n", req_header->url);
  return 0;
}

static int client_header_fieldcb(http_parser *parser, const char *data, size_t len) 
{
  struct client* client = (struct client*)parser->data;

  struct header* req_header = client->pending_request_header ? client->pending_request_header : 
                              client->request_header;

  header_append_field(req_header, data, len);
  return 0;
}

static int client_header_valuecb(http_parser *parser, const char *data, size_t len)
{ 
  struct client* client = (struct client*)parser->data;
  struct header* req_header = client->pending_request_header ? client->pending_request_header : 
                              client->request_header;

  header_append_value(req_header, data, len);
  return 0;
}

static int client_headers_completecb(http_parser *parser)
{
  struct client* client = (struct client*)parser->data;

    struct header* req_header = client->pending_request_header ? client->pending_request_header : 
                              client->request_header;

  header_set_method(req_header, http_method_str_((enum http_method)parser->method));
  header_append_complete(req_header);
  ep_file_detect(client->file, 0, -1);
  client->relay_sys = &http2_relay_sys;
  if(req_header == client->request_header) {
    char host[255] = {};
    int port = header_get_host_port(client->request_header);
    header_get_host(client->request_header, host, sizeof(host));
    client->relay = client->relay_sys->relay_create(host, port, client_relay_cb, client);
  }

  return 0;
}

static int client_bodycb(http_parser *parser, const char *data, size_t len) {
  struct client* client = (struct client*)parser->data;
  fprintf(stderr, "client_bodycb:%d\n", len);

  if(client->relay_sys->relay_get_status(client->relay) < RELAY_STATUS_CONNECTED) {
    struct ep_buffer* buf = ep_buffer_create((char*)data, len);
    list_add_tail(&client->list_request_body_buffer, &buf->list);
    return 0;
  }

  if(client->relay) {
    client->relay_sys->relay_write_body(client->relay, (char*)data, len);
  }
  
  return 0;
}

static int client_message_completecb(http_parser *parser)
{
  fprintf(stderr, "client %p message complete", parser->data);
  struct client* client = (struct client*)parser->data;
  client->relay_sys->relay_complete_request(client->relay);
  return 0;
}

static http_parser_settings htp_hooks = {
  client_msg_begin, /*http_cb      on_message_begin;*/
  client_url_complete, /*http_data_cb on_url;*/
  NULL, /*http_cb on_status_complete */
  client_header_fieldcb, /*http_data_cb on_header_field;*/
  client_header_valuecb, /*http_data_cb on_header_value;*/
  client_headers_completecb, /*http_cb      on_headers_complete;*/
  client_bodycb, /*http_data_cb on_body;*/
  client_message_completecb /*http_cb      on_message_complete;*/
};

int client_init(struct ep_base* base) {
  s_base = base;
  http2_relay_sys.relay_sys_init(base);
  http_relay_sys.relay_sys_init(base);
  INIT_LIST_HEAD(&s_list_clients);
  return 0;
}

void client_fini() {

}

static void client_readcb(struct client* client) {
  char recv_buf[RECV_BUFFER_SIZE] = {};
  int fd = client->client_fd;
  int len_read = recv(fd, recv_buf, sizeof(recv_buf), 0);
  if(len_read <= 0) {
    fprintf(stderr, "client %d read -1, errno=%d\n", fd, EAGAIN);
    client_drop(client);
    return;
  }

  int n_parsed = http_parser_execute_(&client->parser, &htp_hooks, recv_buf, len_read);
}

static void client_writecb(struct client* client) {
  bool is_error = false;
  int detect = ep_file_get_detect(client->file);
  int timeout = ep_file_get_timeout(client->file);
  assert(detect & EP_WRITE);
  struct client_write_buffer* pos = NULL;
  struct client_write_buffer* n = NULL;
  if(list_empty(&client->list_response_write_buffer)) {
    detect &= (!EP_WRITE);
    client->relay_sys->relay_resume_read(client->relay);
    return;
  }

  list_for_each_entry_safe(pos, n, &client->list_response_write_buffer, list) {
    int len_write = send(client->client_fd, (char*)pos->buf + pos->len_sent, pos->len_total - pos->len_sent, 0);
    if(len_write < 0) {
      if(errno != EINPROGRESS) {
        is_error = true;
        break;
      }
    } else if(len_write >= 0 && len_write < (pos->len_total - pos->len_sent)) {
      pos->len_sent += len_write;
      client->pending_write -= len_write;
      break;
    } else {
      client->pending_write -= len_write;
      list_del(&pos->list);
      free(pos->buf);
      free(pos);
    }
  }

  if(is_error) {
    client_drop(client);
    return;
  }

  if(list_empty(&client->list_response_write_buffer)) {
    detect &= (!EP_WRITE);
    ep_file_detect(client->file, detect, timeout);
  }

  if(client->pending_write < 30000) {
    client->relay_sys->relay_resume_read(client->relay);
  }
}

static void client_eventcb(struct ep_file* file, int fd, short what, void* args) {
  fprintf(stderr, "client_eventcb,fd=%d, what=%d\n", fd, what);
  if(what == EP_READ) {
    client_readcb((struct client*)args);
  } else if(what & EP_ERROR) {
    client_drop((struct client*)args);
  } else if(what & EP_WRITE) {
    client_writecb((struct client*)args);
  }
}

void client_handle_fd(int fd) {
  struct client* client = (struct client*)calloc(1, sizeof(struct client));
  client->client_fd = fd;
  client->file = ep_file_create(s_base, fd, client_eventcb, client);
  ep_file_detect(client->file, EP_READ, 30 * 1000);

  http_parser_init_(&client->parser, HTTP_REQUEST);
  client->parser.data = client;
  list_add_tail(&client->list, &s_list_clients);
  INIT_LIST_HEAD(&client->list_response_write_buffer);
  INIT_LIST_HEAD(&client->list_request_body_buffer);
  fprintf(stderr, "accept client:%p, fd=%d", client, fd);
}

void client_drop(struct client* client) {
  
  fprintf(stderr, "dropping client:%p\n", client);
  struct client_write_buffer* buffer = NULL;
  struct client_write_buffer* tmp = NULL;
  list_del(&client->list);

  list_for_each_entry_safe(buffer, tmp, &client->list_response_write_buffer, list) {
    list_del(&buffer->list);
    free(buffer->buf);
    free(buffer);
  }

  if(client->relay) {
    client->relay_sys->relay_close(client->relay);
  }

  if(client->request_header) {
    header_free(client->request_header);
  }

  if(client->pending_request_header) {
    header_free(client->pending_request_header);
  }

  if(client->file) {
    ep_file_detect(client->file, 0, -1);
    ep_file_free(client->file);
    client->file = NULL;
  }

  if(client->client_fd) {
    close(client->client_fd);
    client->client_fd = 0;
  }

  free(client);
}
