#include "header.h"
#include "list.h"
#include "ep.h"
struct downstream {};
typedef void (*downstream_cb)(short what, struct downstream* stream, void* args);
typedef void (*downstream_setargs)(struct downstream* stream, void* args);
typedef int (*downstream_init)(struct ep_base* base, int port, downstream_cb cb);
typedef struct header* (*downstream_get_request_header)(struct downstream* stream);
typedef void (*downstream_send_response)(struct header* response_header);
typedef void (*downstream_close)(struct downstream* stream);
typedef list_head* (*downstream_get_body)(struct downstream* stream);
typedef int (*downstream_write_body)(struct downstream* stream, const char* buf, int len);
typedef int (*downstream_get_status)(struct downstream* stream);
typedef int (*downstream_get_write_buffer_len)(struct downstream* stream);

struct upsream_sys {
  downstream_init init;
  downstream_setargs setargs;
  downstream_get_request_header get_request_header;
  downstream_send_response send_response;
  downstream_close close;
  downstream_get_body get_body;
  downstream_write_body write_body;
  downstream_get_status get_status;
  downstream_get_write_buffer_len get_write_buffer_len;
};

enum {
  DOWNSTREAM_EVENT_NEW,
  DOWNSTREAM_EVENT_BODY,
  DOWNSTREAM_EVENT_REQUEST_COMPLETE,
  DOWNSTREAM_EVENT_DISCONNECT
};


