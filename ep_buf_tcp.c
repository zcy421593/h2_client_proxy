
#include "ep_buf.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include "list.h"
#include "sys_network.h"
#include "ep_buffer.h"
#include "util.h"
#include <errno.h>
#include <assert.h>

struct ep_buf_impl {
  struct ep_buf base_buf;
  struct ep_base* base;
  int fd;
  struct ep_file* file;
  ep_bufcb cb;
  void* args;
  char* buf_read;
  int read_buf_len;

  bool is_incb;
  bool is_free_pending;

  list_head list_write_buf;
  int detect;
};

static void ep_buf_free(struct ep_buf* buf);

static int ep_buf_file_readcb(struct ep_buf_impl* buf_impl) {
  char buf[4096] = {};
  int len_read = read(buf_impl->fd, buf, sizeof(buf));
  buf_impl->buf_read = (char*)buf;
  buf_impl->read_buf_len = len_read;
  buf_impl->is_incb = true;

  if(buf_impl->detect & EP_BUF_READ) {
    buf_impl->is_incb = true;
    buf_impl->cb(&buf_impl->base_buf, EP_BUF_READ, buf_impl->args);
    buf_impl->is_incb = false;
  }

  if(buf_impl->is_free_pending) {
    buf_impl->is_free_pending = false;
    ep_buf_free((struct ep_buf*)buf_impl);
    return -1;
  }
  buf_impl->read_buf_len = -1;
  buf_impl->buf_read = NULL;
  return 0;
}

static int ep_buf_file_writecb(struct ep_buf_impl* buf_impl) {
  int detect = ep_file_get_detect(buf_impl->file);
  int timeout = ep_file_get_timeout(buf_impl->file);
  struct ep_buffer* pos = NULL;
  struct ep_buffer* n = NULL;
  bool is_error = false;
  if(list_empty(&buf_impl->list_write_buf)) {
    fprintf(stderr, "write buffer empty, stop detect write event\n");
    detect &= (~EP_WRITE);
    ep_file_detect(buf_impl->file, detect, timeout);
  } else {
    list_for_each_entry_safe(pos, n, &buf_impl->list_write_buf, list) {
      int len_write = write(buf_impl->fd, pos->buf + pos->len_sent, pos->len_total - pos->len_sent);
      fprintf(stderr, "sending write buffer,res=%d\n", len_write);
      if(len_write < 0) {
        if(errno != EINPROGRESS && errno != EAGAIN) {
          is_error = true;
        }
        break;
      }
      pos->len_sent += len_write;
      if(pos->len_sent == pos->len_sent) {        
        list_del(&pos->list);
        ep_buffer_free(pos);
        break;
      } else {
        break;
      }
    }
  }

  if(list_empty(&buf_impl->list_write_buf)) {
    detect &= (~EP_WRITE);
    buf_impl->is_incb = true;
    ep_file_detect(buf_impl->file, detect, timeout);
    buf_impl->is_incb = false;

    if(buf_impl->is_free_pending) {
      buf_impl->is_free_pending = false;
      ep_buf_free((struct ep_buf*)buf_impl);
      return -1;
    }
  }

  if(is_error) {
    ep_file_detect(buf_impl->file, 0, -1);
    buf_impl->is_incb = true;
    buf_impl->cb(&buf_impl->base_buf, EP_BUF_ERROR, buf_impl->args);
    buf_impl->is_incb = false;

    if(buf_impl->is_free_pending) {
      buf_impl->is_free_pending = false;
      ep_buf_free((struct ep_buf*)buf_impl);
      return -1;
    }
  }

  if(buf_impl->detect & EP_BUF_WRITE) {
    buf_impl->is_incb = true;
    buf_impl->cb(&buf_impl->base_buf, EP_BUF_WRITE, buf_impl->args);
    buf_impl->is_incb = false;

    if(buf_impl->is_free_pending) {
      buf_impl->is_free_pending = false;
      ep_buf_free((struct ep_buf*)buf_impl);
      return -1;
    }
  }
  return 0;

}

static void ep_buf_file_eventcb(struct ep_file* file, int fd, short what, void* args) {
  fprintf(stderr, "ep_buf_file_eventcb:%d\n", what);
  struct ep_buf_impl* buf_impl = (struct ep_buf_impl*)args;


  if((what & EP_WRITE)) {
    fprintf(stderr, "ep_buf_file_eventcb:write\n");
    if(ep_buf_file_writecb(buf_impl) !=0) {
      return;
    }
  }

  if((what & EP_READ)) {
    fprintf(stderr, "ep_buf_file_eventcb:read\n");
    if(ep_buf_file_readcb(buf_impl) != 0) {
      return;
    }
  }

    if(what & EP_ERROR) {
    ep_file_detect(buf_impl->file, 0, -1);
    buf_impl->cb(&buf_impl->base_buf, EP_BUF_ERROR, buf_impl->args);
    return;
  }
}

static struct ep_buf* ep_buf_create(struct ep_base* base, int fd, ep_bufcb cb, void* args) {


  struct ep_buf_impl* buf = (struct ep_buf_impl*)calloc(1, sizeof(struct ep_buf_impl));

  if(fd == -1) {
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    make_socket_nonblocking(fd);
  }

  buf->base = base;
  buf->fd = fd;
  buf->cb = cb;
  buf->args = args;
  buf->file = ep_file_create(base, fd, ep_buf_file_eventcb, buf);
  INIT_LIST_HEAD(&buf->list_write_buf);
  return &buf->base_buf;
}

static int ep_buf_connect(struct ep_buf* buf, const char* ip, int port) {
  fprintf(stderr, "connect %s:%d\n", ip, port);
  struct ep_buf_impl* buf_impl = (struct ep_buf_impl*)buf;
  struct sockaddr_in addr = {};
  addr.sin_addr.s_addr = inet_addr(ip);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  int ret = connect(buf_impl->fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
  if(ret !=0 && errno != EINPROGRESS) {
    fprintf(stderr, "connect %s err\n", ip);
    return -1;
  }
  return 0;
}

static void ep_buf_read(struct ep_buf* buf_file, char** pbuf, int* len) {
  struct ep_buf_impl* buf_impl = (struct ep_buf_impl*)buf_file;
  assert(buf_impl);
  assert(buf_impl->is_incb);
  *pbuf = buf_impl->buf_read;
  *len = buf_impl->read_buf_len;
}

static void ep_buf_write(struct ep_buf* buf_file, char* buf, int len) {
  fprintf(stderr, "ep_buf_write, len=%d\n", len);
  struct ep_buf_impl* buf_impl = (struct ep_buf_impl*)buf_file;
  int detect = ep_file_get_detect(buf_impl->file);
  int timeout = ep_file_get_timeout(buf_impl->file);
  if(list_empty(&buf_impl->list_write_buf)) {
    fprintf(stderr, "write buf empty, sending\n");
    int write_len = send(buf_impl->fd, buf, len, 0);

    if(write_len >= 0 && write_len < len) {
      struct ep_buffer* writebuf = ep_buffer_create(buf + write_len, len - write_len);
      list_add_tail(&writebuf->list, &buf_impl->list_write_buf);
      detect |= EP_WRITE;
      ep_file_detect(buf_impl->file, detect, timeout);
    }
  } else {
    fprintf(stderr, "write buf not empty, saving\n");
     struct ep_buffer* writebuf = ep_buffer_create(buf, len);
     list_add_tail(&writebuf->list, &buf_impl->list_write_buf);

     detect |= EP_WRITE;
      ep_file_detect(buf_impl->file, detect, timeout);
  }
}

static void ep_buf_enable(struct ep_buf* buf_file, short what) {
  struct ep_buf_impl* buf_impl = (struct ep_buf_impl*)buf_file;
  int timeout = ep_file_get_timeout(buf_impl->file);
  int detect = ep_file_get_detect(buf_impl->file);
  if(what & EP_BUF_READ) {
    detect |= EP_READ;
  } else if(what & EP_BUF_WRITE) {
    detect |= EP_WRITE;
  }
  ep_file_detect(buf_impl->file, detect, timeout);
  buf_impl->detect = what;
}

static void ep_buf_disable(struct ep_buf* buf_file, short what) {
  struct ep_buf_impl* buf_impl = (struct ep_buf_impl*)buf_file;
  int timeout = ep_file_get_timeout(buf_impl->file);
  int detect = ep_file_get_detect(buf_impl->file);

  if(what & EP_BUF_READ) {
    detect &= (!EP_READ);
  } else if(what & EP_BUF_WRITE) {
    detect &= (!EP_WRITE);
  }

  ep_file_detect(buf_impl->file, detect, timeout);
  buf_impl->detect = what;
}

void ep_buf_set_timeout(struct ep_buf* buf_file, int ms) {
  struct ep_buf_impl* buf_impl = (struct ep_buf_impl*)buf_file;
  int detect = ep_file_get_detect(buf_impl->file);
  ep_file_detect(buf_impl->file, detect, ms);
}

int ep_buf_get_output_len(struct ep_buf* buf_file) {
  struct ep_buf_impl* buf_impl = (struct ep_buf_impl*)buf_file;
  struct ep_buffer* pos = NULL;
  int res = 0;
  list_for_each_entry(pos, &buf_impl->list_write_buf, list) {
    res += (pos->len_total - pos->len_sent);
  }
  return res;
}

static void ep_buf_free(struct ep_buf* buf) {
  struct ep_buf_impl* buf_impl = (struct ep_buf_impl*)buf;
  struct ep_buffer* pos = NULL;
  struct ep_buffer* n = NULL;

  if(buf_impl->is_incb) {
    buf_impl->is_free_pending = true;
    return;
  }


  list_for_each_entry_safe(pos, n, &buf_impl->list_write_buf, list) {
    list_del(&pos->list);
    ep_buffer_free(pos);
  }
  ep_file_detect(buf_impl->file, 0, -1);
  close(buf_impl->fd);
  ep_file_free(buf_impl->file);
  free(buf_impl);
}

struct ep_buf_proto ep_buf_proto_tcp = {
  .create = ep_buf_create,
  .connect = ep_buf_connect,
  .read = ep_buf_read,
  .write = ep_buf_write,
  .enable = ep_buf_enable,
  .disable = ep_buf_disable,
  .set_timeout = ep_buf_set_timeout,
  .get_output_len = ep_buf_get_output_len,
  .free = ep_buf_free,
};
