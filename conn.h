#ifndef CONN_H
#define CONN_H
struct ep_base;
struct conn {};
typedef void (*conn_cb)(struct conn*, short what, void* args);
typedef struct conn * (*conn_create_ptr)(struct ep_base* base, const char* host, int port, conn_cb cb,void* args);
typedef int (*conn_read_ptr)(struct conn*, char* buf, int len);
typedef int (*conn_write_ptr)(struct conn*, char* buf, int len);
typedef void (*conn_close_ptr)(struct conn*);
typedef void (*conn_detect_ptr)(struct conn*, short what, int timeout);

struct conn_proto {
  conn_create_ptr conn_create;
  conn_read_ptr conn_read;
  conn_write_ptr conn_write;
  conn_close_ptr conn_close;
  conn_detect_ptr conn_detect;
};

enum {
  CONN_NONE = 0,
  CONN_READ = 1,
  CONN_WRITE = 2,
  CONN_ERROR = 4, 
  CONN_TIMEOUT =8
};
#endif // CONN_H
