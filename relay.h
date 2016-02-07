#ifndef RELAY_H
#define RELAY_H
struct ep_base;
struct header;
typedef void (*relay_eventcb)(void* relay, short what, void* args);
typedef int (*relay_sys_init_ptr)(struct ep_base* base);
typedef void* (*relay_create_ptr)(const char* host, int port, relay_eventcb cb, void* args);
typedef void (*relay_send_request_ptr)(void* relay, const struct header* header);
typedef struct header* (*relay_get_header_ptr)(void* relay);
typedef int (*relay_get_body_ptr)(void* relay, char** ptr_bldy, int* len_body);
typedef int (*relay_write_body_ptr)(void* relay, char* data, int len);
typedef void (*relay_close_ptr)(void* relay);
typedef int (*relay_get_status_ptr)(void* relay);
typedef void (*relay_suspend_read_ptr)(void* relay);
typedef void (*relay_resume_read_ptr)(void* relay);
typedef void (*relay_complete_request_ptr)(void* relay);
enum {
  RELAY_CONNECTED,
  RELAY_HEADER_COMPLETE,
  RELAY_BODY,
  RELAY_MSG_COMPLETE,
  RELAY_ERR_DNS_FAILED,
  RELAY_ERR_CONN_FAILED,
  RELAY_ERR_RESPONSE_PARSE_FAILED,
  RELAY_ERR_CONN_TERMINATE
};

enum {
  RELAY_STATUS_CONNECTING,
  RELAY_STATUS_CONNECTED,
  RELAY_STATUS_REQUEST_SENT,
  RELAY_STATUS_HEADER_COMPLETE,
  RELAY_STATUS_MSG_COMPLETE
};

struct relay_sys {
  relay_sys_init_ptr relay_sys_init;
  relay_create_ptr relay_create;
  relay_send_request_ptr relay_send_request;
  relay_get_header_ptr relay_get_header;
  relay_get_body_ptr relay_get_body;
  relay_write_body_ptr relay_write_body;
  relay_close_ptr relay_close;
  relay_get_status_ptr relay_get_status;
  relay_suspend_read_ptr relay_suspend_read;
  relay_resume_read_ptr relay_resume_read;
  relay_complete_request_ptr relay_complete_request;
};

#endif //RELAY_H
