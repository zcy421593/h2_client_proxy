#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "sys_network.h"
#include "util.h"
#include "downstream.h"
#include "ep_buf.h"
#include "http_parser.h"
extern struct ep_buf_proto ep_buf_proto_tcp;

struct http_downstream {
  list_head list;
  list_head list_events;
  list_head list_request_bodys;
  struct downstream base;
  int fd;
  struct ep_buf* client;
  struct header* request_header;
  struct header* resposne_header;
  http_parser request_parser;
  void* args;
  bool is_close_pending;
  bool is_incb;
  bool is_response_complete;
};

struct event_item {
  list_head list;
  struct ep_timer* timer;
  struct http_downstream* downstream;
  short what;
};

static struct ep_base* s_base = NULL;
static int s_listen_fd = 0;
static downstream_cb s_cb = NULL;
static struct ep_file* s_file_listen = NULL;
static list_head s_list_downstreams = LIST_HEAD_INIT(s_list_downstreams);

static void http_upstream_queue_event(struct http_downstream* stream, int what);

static int http_downstream_msg_begin(http_parser* parser) {  
  return 0;
}

static int http_downstream_url_complete(http_parser* parser, const char *data, size_t len)
{
  struct http_downstream* stream = (struct http_downstream*)parser->data;
  header_set_url(stream->request_header, data, len);
  return 0;
}

static int http_downstream_header_fieldcb(http_parser *parser, const char *data, size_t len) 
{
  struct http_downstream* stream = (struct http_downstream*)parser->data;
  header_append_field(stream->request_header, data, len);
  return 0;
}

static int http_downstream_header_valuecb(http_parser *parser, const char *data, size_t len)
{
  struct http_downstream* stream = (struct http_downstream*)parser->data;
  header_append_value(stream->request_header, data, len);
  return 0;
}

static int http_downstream_headers_completecb(http_parser *parser)
{
  struct http_downstream* stream = (struct http_downstream*)parser->data;
  header_append_complete(stream->request_header);
  http_upstream_queue_event(stream, DOWNSTREAM_EVENT_NEW);
  fprintf(stderr, "http_downstream_headers_completecb\n");
  return 0;
}

static int http_downstream_bodycb(http_parser *parser, const char *data, size_t len) {
  struct http_downstream* stream = (struct http_downstream*)parser->data;
  return 0;
}

static int http_downstream_message_completecb(http_parser *parser)
{
  struct http_downstream* stream = (struct http_downstream*)parser->data;
  return 0;
}


static http_parser_settings htp_hooks = {
  http_downstream_msg_begin, /*http_cb      on_message_begin;*/
  http_downstream_url_complete, /*http_data_cb on_url;*/
  NULL, /*http_cb on_status_complete */
  http_downstream_header_fieldcb, /*http_data_cb on_header_field;*/
  http_downstream_header_valuecb, /*http_data_cb on_header_value;*/
  http_downstream_headers_completecb, /*http_cb      on_headers_complete;*/
  http_downstream_bodycb, /*http_data_cb on_body;*/
  http_downstream_message_completecb /*http_cb      on_message_complete;*/
};


static void http_upstream_eventcb(struct ep_timer* timer, void* args) {
  struct event_item* item = (struct event_item*)args;
  struct http_downstream* stream = item->downstream;
  item->downstream->is_incb = true;

  if(s_cb) {
    s_cb(item->what, &item->downstream->base, item->downstream->args);
  }
  
  item->downstream->is_incb = false;

  list_del(&item->list);
  ep_timer_free(item->timer);
  free(item);

  if(stream->is_close_pending) {
    // 释放空间
  }
}

static void http_upstream_queue_event(struct http_downstream* stream, int what) {
  struct event_item* item = (struct event_item*)calloc(1, sizeof(struct event_item));
  item->downstream = stream;
  item->what = what;
  item->timer = ep_timer_new(s_base, http_upstream_eventcb, item);
  list_add(&item->list, &stream->list_events);
  ep_timer_add(item->timer, 0);
}

static void http_upstream_clientcb(struct ep_buf* buf_file, short what, void* args) {
  struct http_downstream* stream = (struct http_downstream*)args;
  if(what == EP_BUF_READ) {
    char* buf;
    int len;
    ep_buf_proto_tcp.read(buf_file, &buf, &len);

    if(len <= 0) {
      return;
    }
    int nparsed = http_parser_execute_(&stream->request_parser, &htp_hooks, buf, len);
    
  } else if(what == EP_BUF_WRITE) {

  } else if(what == EP_BUF_ERROR) {

  }
}

static void http_upstream_acceptcb(struct ep_file* file, int fd, short what, void* args) {
  struct sockaddr_in addr = {};
  socklen_t addr_len = sizeof(struct sockaddr_in);
  int client_fd = accept(fd, (struct sockaddr*)&addr, &addr_len);
  struct http_downstream* new_stream = (struct http_downstream*)calloc(1, sizeof(struct http_downstream));
  new_stream->fd = client_fd;
  new_stream->client = ep_buf_proto_tcp.create(s_base, client_fd, http_upstream_clientcb, new_stream);
  ep_buf_proto_tcp.enable(new_stream->client, EP_BUF_READ);

  http_parser_init_(&new_stream->request_parser, HTTP_REQUEST);
  new_stream->request_parser.data = new_stream;

  INIT_LIST_HEAD(&new_stream->list_events);
  INIT_LIST_HEAD(&new_stream->list_request_bodys);

  new_stream->request_header = header_new(HEADER_REQUEST);
  new_stream->is_response_complete = true;

  list_add_tail(&new_stream->list, &s_list_downstreams);
}

static void http_downstream_setargs(struct downstream* stream, void* args) {
  struct http_downstream* http_stream = (struct http_downstream*)stream;
  assert(stream);
  http_stream->args = args;
}

static int http_downstream_init(struct ep_base* base, int port, downstream_cb cb) {
  s_listen_fd = server_create_listen_fd(port);
  s_base = base;

  if(s_listen_fd <= 0) {
    goto error;
  }
  if(make_socket_nonblocking(s_listen_fd) != 0) {
    goto error;
  }

  s_file_listen = ep_file_create(base, s_listen_fd, http_upstream_acceptcb, NULL);
  ep_file_detect(s_file_listen, EP_READ, -1);
  s_cb = cb;
  return 0;
error:
  if(s_listen_fd > 0) {
    close(s_listen_fd);
    s_listen_fd = 0;
  }
  s_cb = NULL;
  return -1;
}

static struct header* http_downstream_get_request_header(struct downstream* stream) {
  return NULL;
}
static void http_downstream_send_response(struct header* response_header) {

}
static void http_downstream_close(struct downstream* stream) {

}
static list_head* http_downstream_get_body(struct downstream* stream) {
  return NULL;
}
static int http_downstream_write_body(struct downstream* stream, const char* buf, int len) {
  return 0;
}
static int http_downstream_get_status(struct downstream* stream) {
  return 0;
}
static int http_downstream_get_write_buffer_len(struct downstream* stream) {
  return 0;
}

struct upsream_sys http_upsteam = {
  .init = http_downstream_init,
  .setargs = http_downstream_setargs,
  .get_request_header = http_downstream_get_request_header,
  .send_response = http_downstream_send_response,
  .close = http_downstream_close,
  .get_body = http_downstream_get_body,
  .write_body = http_downstream_write_body,
  .get_status = http_downstream_get_status,
  .get_write_buffer_len = http_downstream_get_write_buffer_len,
};