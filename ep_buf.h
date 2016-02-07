#ifndef EP_BUF_H
#define EP_BUF_H
#include "ep.h"

struct ep_buf {};
typedef void (*ep_bufcb)(struct ep_buf* buf_file, short what, void* args);
typedef struct ep_buf* (*ep_buf_create_ptr)(struct ep_base* base, int fd, ep_bufcb cb, void* args);
typedef int (*ep_buf_connect_ptr)(struct ep_buf* buf_file, const char* ip, int port);
typedef void (*ep_buf_read_ptr)(struct ep_buf* buf_file, char** pbuf, int* len);
typedef void (*ep_buf_write_ptr)(struct ep_buf* buf_file, char* buf, int len);
typedef void (*ep_buf_enable_ptr)(struct ep_buf* buf_file, short what);
typedef void (*ep_buf_disable_ptr)(struct ep_buf* buf_file, short what);
typedef void (*ep_buf_set_timeout_ptr)(struct ep_buf* buf_file, int ms);
typedef int (*ep_buf_get_output_len_ptr)(struct ep_buf* buf_file);
typedef void (*ep_buf_free_ptr)(struct ep_buf* buf);

enum {
  EP_BUF_READ = 1,
  EP_BUF_WRITE =2 ,
  EP_BUF_ERROR = 4,
  EP_BUF_TIMEOUT = 8
};

struct ep_buf_proto {
  ep_buf_create_ptr create;
  ep_buf_connect_ptr connect;
  ep_buf_read_ptr read;
  ep_buf_write_ptr write;
  ep_buf_enable_ptr enable;
  ep_buf_disable_ptr disable;
  ep_buf_set_timeout_ptr set_timeout;
  ep_buf_get_output_len_ptr get_output_len;
  ep_buf_free_ptr free;
};

#endif