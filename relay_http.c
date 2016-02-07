#include "relay.h"
#include "http_parser.h"
#include "ep_buf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "header.h"
#include "util.h"
#include "sys_network.h"
#include "dns.h"
#include "list.h"
#include "ep.h"
#include <assert.h>

#define HOST_LENGTH 255
#define IDLE_RELAY_BUCKET_COUNT 512

typedef struct http_relay {
  list_head list;
  char host[HOST_LENGTH];
  int port;
  http_parser parser;
  struct ep_buf_proto* conn_proto;
  struct ep_buf* conn;
  struct dns_req* dns_req;
  struct header* response_header;
  struct header* request_header;
  struct ep_timer* tmr_connected;

  bool is_chunk_post;
  bool connected;
  bool in_body_callback;

  relay_eventcb cb;
  void* args;
  char* ptr_body;
  int cur_body_len;
  bool is_idle;

  bool should_free;
  int relay_status;
} http_relay;

extern struct ep_buf_proto ep_buf_proto_tcp;
static struct ep_base* s_base = NULL;
static void relay_conncb(struct ep_buf* conn, short what, void* args);
static bool relay_has_length_info(struct http_relay* relay);

static list_head s_idle_relay_bucket[IDLE_RELAY_BUCKET_COUNT] = {};

static int relay_count_hash(const char* host) {
  uint32_t res = 0;
  int i = 0;
  for(i = 0; i < strlen(host); i++) {
    res = res +  host[i];
  }
  return res % IDLE_RELAY_BUCKET_COUNT;
}

static int http_relay_sys_init(struct ep_base* base) {
  int i = 0;
  s_base = base;

  for(i = 0; i< IDLE_RELAY_BUCKET_COUNT; i++) {
    INIT_LIST_HEAD(&s_idle_relay_bucket[i]);
  }
  return 0;
}

static void relay_sys_reuse_relay(struct http_relay* r) {

  struct http_relay* relay = (struct http_relay*)r;
  int hash = relay_count_hash(relay->host);

  assert(!relay->should_free);
  assert(!relay->is_idle);
  assert(!relay->dns_req);
  assert(relay->conn); 

  relay->relay_status = RELAY_STATUS_CONNECTED;
  relay->args = NULL;
  relay->is_idle = true;
  if(relay->response_header) {
    header_free(relay->response_header);
    relay->response_header = NULL;
  }

  list_add_tail(&relay->list, &s_idle_relay_bucket[hash]);

}

static struct http_relay* relay_sys_find_alive_relay(const char* host, int port) {
  int hash = relay_count_hash(host);
  struct http_relay* pos = NULL;
  struct http_relay* tmp = NULL;
  list_for_each_entry_safe(pos, tmp, &s_idle_relay_bucket[hash], list) {
    if(strcasecmp(pos->host, host) == 0 && pos->port == port) {
      pos->is_idle = false;
      list_del(&pos->list);
      return pos;
    }
  }
  return NULL;
}

static void relay_free(struct http_relay* relay) {
  if(relay->response_header) {
    header_free(relay->response_header);
  }

  if(relay->is_idle) {
    list_del(&relay->list);
  }

  if(relay->conn) {
    relay->conn_proto->free(relay->conn);
  }

  if(relay->dns_req) {
    dns_cancel(relay->dns_req);
  }

  if(relay->tmr_connected) {
    if(ep_timer_pending(relay->tmr_connected)) {
      ep_timer_del(relay->tmr_connected);
    }
    ep_timer_free(relay->tmr_connected);
  }

  free(relay);
}

static int relay_message_begincb(http_parser *parser) {
  struct http_relay* relay = (struct http_relay*)parser->data;
  relay->response_header = header_new(HEADER_RESPONSE);
   return 0;
}

static int relay_status_completecb(http_parser *parser) {
  struct http_relay* relay = (struct http_relay*)parser->data;
  header_set_status_code(relay->response_header, parser->status_code);
   return 0;
}

static int relay_url_complete(http_parser* parser, const char *data, size_t len)
{
  struct http_relay* relay = (struct http_relay*)parser->data;
  header_set_url(relay->response_header, data, len);
  return 0;
}

static int relay_header_fieldcb(http_parser *parser, const char *data, size_t len) 
{
  struct http_relay* relay = (struct http_relay*)parser->data;
  header_append_field(relay->response_header, data, len);
  return 0;
}

static int relay_header_valuecb(http_parser *parser, const char *data, size_t len)
{ 
  struct http_relay* relay = (struct http_relay*)parser->data;
  header_append_value(relay->response_header, data, len);
  return 0;
}

static int relay_headers_completecb(http_parser *parser)
{
  struct http_relay* relay = (struct http_relay*)parser->data;
  header_append_complete(relay->response_header);
  relay_has_length_info(relay);
  relay->relay_status = RELAY_HEADER_COMPLETE;
  relay->cb(relay, RELAY_HEADER_COMPLETE, relay->args);
  return 0;
}

static int relay_bodycb(http_parser *parser, const char *data, size_t len) {
  fprintf(stderr, "relay_bodycb\n");
  struct http_relay* relay = (struct http_relay*)parser->data;
  relay->in_body_callback = true;
  relay->ptr_body = (char*)data;
  relay->cur_body_len = len;
  relay->cb(relay, RELAY_BODY, relay->args);
  relay->in_body_callback = false;
  relay->ptr_body = NULL;
  relay->cur_body_len = -1;
  return 0;
}

static int relay_message_completecb(http_parser *parser)
{
  struct http_relay* relay = (struct http_relay*)parser->data;
  relay->relay_status = RELAY_STATUS_CONNECTED;
  relay->cb(relay, RELAY_MSG_COMPLETE, relay->args);
  relay_sys_reuse_relay(relay);
  return 0;
}

static http_parser_settings htp_hooks = {
  relay_message_begincb,                     //http_cb      on_message_begin
  relay_url_complete,       //http_data_cb on_url;
  relay_status_completecb,  //http_cb on_status_complete 
  relay_header_fieldcb,     //http_data_cb on_header_field;
  relay_header_valuecb,     //http_data_cb on_header_value;
  relay_headers_completecb, //http_cb      on_headers_complete;
  relay_bodycb,             //http_data_cb on_body;*/
  relay_message_completecb  //http_cb      on_message_complete;
};

static void relay_dns_resolved(int code, const char** ips, int num, const char* cname, void* args) {
  
  struct http_relay* relay = (struct http_relay*)args;

  relay->dns_req = NULL;
  if(code != 0) {
    relay->cb(relay, RELAY_ERR_DNS_FAILED, relay->args);
    relay_free(relay);
    return;
  }

  fprintf(stderr, "relay_dns_resolved, num=%d, 1st ip=%s, cname=%s\n",num, ips[0], cname);

  relay->conn_proto = &ep_buf_proto_tcp;
  relay->conn = relay->conn_proto->create(s_base, -1, relay_conncb, relay);
  relay->conn_proto->connect(relay->conn, ips[0], relay->port);
  relay->conn_proto->enable(relay->conn, EP_BUF_WRITE);
}

static bool relay_has_length_info(struct http_relay* relay) {
  const char* content_length = header_value(relay->response_header, "content-length");
  if(content_length) {
    return true;
  } else {
    return false;
  }
}

static void relay_handle_conn_err(struct http_relay* relay) {
  relay->conn_proto->free(relay->conn);
  relay->conn = NULL;

  // no idle, notify
  if(!relay->is_idle) {
    if(relay->response_header && !relay_has_length_info(relay)) {
      relay->cb(relay, RELAY_MSG_COMPLETE, relay->args);
    } else if(relay->relay_status < RELAY_STATUS_CONNECTED) {
      relay->cb(relay, RELAY_ERR_CONN_FAILED, relay->args);
    } else {
      relay->cb(relay, RELAY_ERR_CONN_TERMINATE, relay->args);
    }
  }
  
  relay_free(relay);
}

static void relay_conn_tmrcb(struct ep_timer* timer, void* args) {
  struct http_relay* relay = (struct http_relay*)args;
  if(relay->tmr_connected) {
    ep_timer_free(relay->tmr_connected);
    relay->tmr_connected = NULL;
  }
  
  relay->tmr_connected = NULL;
  relay->cb(relay, RELAY_CONNECTED, relay->args);
}

static void relay_conncb(struct ep_buf* buf_file, short what, void* args) {
  struct http_relay* relay = (struct http_relay*)args;
  if(!relay->connected && what == EP_BUF_WRITE) {
    fprintf(stderr, "relay connected\n");
    relay->connected = true;
    relay->cb(relay, RELAY_CONNECTED, relay->args);
    fprintf(stderr, "enable read\n");
    relay->conn_proto->enable(relay->conn, EP_BUF_READ);

  } else if(what == EP_BUF_WRITE) {

  } else if(what == EP_BUF_READ) {
    fprintf(stderr, "relay readcb\n");
    char *read_buf = NULL;
    int len = 0;
    relay->conn_proto->read(relay->conn, &read_buf, &len);

    if(len <= 0 || relay->is_idle) {
      relay_handle_conn_err(relay);
      return;
    }

    int nparsed = http_parser_execute_(&relay->parser, &htp_hooks, read_buf, len);

    if(relay->parser.http_errno != HPE_OK && !relay->is_idle) {
      relay->cb(relay, RELAY_ERR_RESPONSE_PARSE_FAILED, relay->args);
      relay_free(relay);
    } else if(relay->should_free) {
      relay_free(relay);
    }
  } else if(what & EP_BUF_ERROR) {
    relay_handle_conn_err(relay);
  }
}

static void* relay_create(const char* host, int port, relay_eventcb cb, void* args) {
  http_relay* relay = relay_sys_find_alive_relay(host, port);
  if(!relay) {
    relay = (http_relay*)calloc(1, sizeof(http_relay));
    strncpy(relay->host, host, sizeof(relay->host));
    relay->port = port;
    relay->dns_req = dns_resolve(host, relay_dns_resolved, relay);
    relay->relay_status = RELAY_STATUS_CONNECTING;
  }
  
  relay->cb = cb;  
  relay->args = args;
  http_parser_init_(&relay->parser, HTTP_RESPONSE);
  relay->parser.data = relay;

  if(relay->relay_status == RELAY_STATUS_CONNECTED) {
    relay->conn_proto->enable(relay->conn, EP_BUF_READ);
    relay->tmr_connected = ep_timer_new(s_base, relay_conn_tmrcb, relay);
    ep_timer_add(relay->tmr_connected, 0);
  }  
  return relay;
}

static void on_responsecb(struct ep_timer* timer, void* args) {
   struct http_relay* http_relay = (struct http_relay*)args;
   const char* succ_response = "HTTP/1.1 200 Connection Established\r\n\r\n";
    http_parser_execute_(&http_relay->parser, &htp_hooks, (char*)succ_response, strlen(succ_response));
    ep_timer_free(timer);
}

static void relay_send_request(void* relay, const struct header* header) {
  bool via_proxy = false;
  struct http_relay* http_relay = (struct http_relay*)relay;
  char url[1024] = {};
  struct http_parser_url ups = {};
  char buf[20 * 1024] = {};
  int len = sizeof(buf);
  struct field* pos = NULL;

  if(strcasecmp(header->method, "connect") == 0) {
    http_relay->relay_status = RELAY_STATUS_CONNECTED;
    struct ep_timer* tmr = ep_timer_new(s_base, on_responsecb, http_relay);
    ep_timer_add(tmr, 0);
    return;
  }

  http_relay->request_header = (struct header*)header;
  const char* request_transfer_encoding = header_value((struct header*)header, "Transfer-Encoding");
  if(request_transfer_encoding && strcasecmp(request_transfer_encoding, "chunked") == 0) {
    http_relay->is_chunk_post = true;
  }

  if(via_proxy) {
    strncpy(url, header->url, sizeof(url));
  } else {
    http_parser_parse_url_(header->url, strlen(header->url), 0, &ups);

    if (ups.field_set & (1 << UF_PATH)) {
      strncpy(url, header->url + ups.field_data[UF_PATH].off, sizeof(url));
    } else {
      strcat(url, "/");

      if (ups.field_set & (1 << UF_QUERY)){
        strcat(url, "?");
        memcpy(url + strlen(url), header->url + ups.field_data[UF_QUERY].off, ups.field_data[UF_QUERY].len);
      }
      if (ups.field_set & (1 << UF_FRAGMENT))
        strcat(url, "#");
        memcpy(url + strlen(url), header->url + ups.field_data[UF_FRAGMENT].off, ups.field_data[UF_FRAGMENT].len);
    }
  }

  append_format(buf, len, "%s %s HTTP/1.1\r\n", header->method, url);
  list_for_each_entry(pos, &header->list_headers, list) {
    if(!via_proxy && strcasecmp(pos->field, "proxy-connection") == 0) {
      append_format(buf, len, "%s: %s\r\n", "Connection", pos->value);
    } else {
      append_format(buf, len, "%s: %s\r\n", pos->field, pos->value);
    }
  }
  strcat(buf, "\r\n");
  fprintf(stderr, "sending request:\n%s", buf);
  http_relay->conn_proto->write(http_relay->conn, buf, strlen(buf));
  http_relay->relay_status = RELAY_STATUS_REQUEST_SENT;
}

static struct header* relay_get_header(void* relay) {
  struct http_relay* http_relay = (struct http_relay*)relay;
  return http_relay->response_header;
}

static int relay_get_body(void* relay, char** ptr_bldy, int* len_body) {
  struct http_relay* http_relay = (struct http_relay*)relay;
  if(!http_relay->in_body_callback) {
    return -1;
  }
  *ptr_bldy = http_relay->ptr_body;
  *len_body = http_relay->cur_body_len;
  return 0;
}

static int relay_write_body(void* relay, char* data, int len) {
  struct http_relay* http_relay = (struct http_relay*)relay;

  if(http_relay->is_chunk_post) {
    char tmp[32] = {};
    snprintf(tmp, sizeof(tmp), "%X\r\n", len);
    http_relay->conn_proto->write(http_relay->conn, tmp, strlen(tmp));
  }

  http_relay->conn_proto->write(http_relay->conn, data, len);

  if(http_relay->is_chunk_post) {
    http_relay->conn_proto->write(http_relay->conn, (char*)"\r\n", strlen("\r\n"));
  }  
  return len;
}

static void relay_close(void* relay) {
  struct http_relay* http_relay = (struct http_relay*)relay;
  relay_free(http_relay);
}

static void relay_suspend_read(void* relay) {
  fprintf(stderr, "relay_suspend_read\n");
  struct http_relay* http_relay = (struct http_relay*)relay;
  http_relay->conn_proto->disable(http_relay->conn, EP_BUF_READ);
}

static void relay_resume_read(void* relay) {
  fprintf(stderr, "relay_resume_read\n");
  struct http_relay* http_relay = (struct http_relay*)relay;
  http_relay->conn_proto->enable(http_relay->conn, EP_BUF_READ);
}

static int relay_get_status(void* relay) {
  struct http_relay* http_relay = (struct http_relay*)relay;
  return http_relay->relay_status;
}

static void relay_complete_request(void* relay) {
  struct http_relay* http_relay = (struct http_relay*)relay;
  const char* str_complete = "0\r\n\r\n";

  if(http_relay->is_chunk_post) {
    http_relay->conn_proto->write(http_relay->conn, (char*)str_complete, 5);
  }
}

struct relay_sys http_relay_sys = {
  .relay_sys_init = http_relay_sys_init,
  .relay_create = relay_create,
  .relay_send_request = relay_send_request,
  .relay_get_header = relay_get_header,
  .relay_get_body = relay_get_body,
  .relay_write_body = relay_write_body,
  .relay_close = relay_close,
  .relay_get_status = relay_get_status,
  .relay_suspend_read = relay_suspend_read,
  .relay_resume_read = relay_resume_read,
  .relay_complete_request =relay_complete_request
};
