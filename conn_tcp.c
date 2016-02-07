#include "conn.h"
#include <stdlib.h>
#include <stdio.h>
#include "ep.h"
#include "sys_network.h"
#include "util.h"
struct tcp_conn {
  struct conn conn;
  conn_cb cb;  
  struct ep_file* file;
  void* args;
  int fd;
};

static void tcp_conn_eventcb(struct ep_file* file, int fd, short what, void* args) {
  struct tcp_conn* tcp_conn = (struct tcp_conn*)args;
  short ev = 0;
  if(what & EP_READ) {
    ev |= EP_READ;
  }

  if(what & EP_WRITE) {
    ev |= CONN_WRITE;
  }

  if(what & EP_TIMEOUT) {
    ev |= CONN_TIMEOUT;
  }

  if(what & EP_ERROR) {
    ev |= CONN_ERROR;
  }
  fprintf(stderr, "tcp_conn_eventcb:%d\n", what);
  tcp_conn->cb(&tcp_conn->conn, ev, tcp_conn->args);

}

static struct conn * tcp_conn_create(struct ep_base* base, const char* host, int port, conn_cb cb,void* args) {
  struct sockaddr_in addr = {};
  struct tcp_conn* tcp_conn = (struct tcp_conn*)calloc(1, sizeof(struct tcp_conn));
  tcp_conn->args = args;
  tcp_conn->cb = cb;
  tcp_conn->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  make_socket_nonblocking(tcp_conn->fd);
  addr.sin_addr.s_addr = inet_addr(host);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  connect(tcp_conn->fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
  tcp_conn->file = ep_file_create(base, tcp_conn->fd, tcp_conn_eventcb, tcp_conn);

  fprintf(stderr, "tcp_conn_create:connecting %s\n", host);
  return &tcp_conn->conn;
}
static int tcp_conn_read(struct conn* conn, char* buf, int len) {
  struct tcp_conn* tcp_conn = (struct tcp_conn*)conn;
  return recv(tcp_conn->fd, buf, len, 0);
}
static int tcp_conn_write(struct conn* conn, char* buf, int len) {
  struct tcp_conn* tcp_conn = (struct tcp_conn*)conn;
  return send(tcp_conn->fd, buf, len, 0);
}
static void tcp_conn_close(struct conn* conn) {
  struct tcp_conn* tcp_conn = (struct tcp_conn*)conn;
  ep_file_detect(tcp_conn->file, 0, -1);
  ep_file_free(tcp_conn->file);
  close(tcp_conn->fd);
  free(tcp_conn);
}

static void tcp_conn_detect(struct conn* conn, short what, int timeout) {
  struct tcp_conn* tcp_conn = (struct tcp_conn*)conn;
  short ev = 0;
  if(what & CONN_READ) {
    ev |= EP_READ;
  }

  if(what & CONN_WRITE) {
    ev |= EP_WRITE;
  }
  ep_file_detect(tcp_conn->file, ev, timeout);
}

struct conn_proto tcp_conn_proto = {
  .conn_create = tcp_conn_create,
  .conn_read = tcp_conn_read,
  .conn_write = tcp_conn_write,
  .conn_detect = tcp_conn_detect,
  .conn_close = tcp_conn_close
};
