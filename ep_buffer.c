#include "ep_buffer.h"
#include <stdlib.h>
#include <assert.h>
#include <memory.h>
struct ep_buffer* ep_buffer_create(char* buf, int len) {
  assert(buf);
  assert(len);
  struct ep_buffer* write_buf = (struct ep_buffer*)calloc(1, sizeof(struct ep_buffer));
  write_buf->buf = (char*)malloc(len);
  memcpy(write_buf->buf, buf, len);
  write_buf->len_total = len;
  INIT_LIST_HEAD(&write_buf->list);
  return write_buf;
}

void ep_buffer_free(struct ep_buffer* buf) {
  if(buf->buf) {
    free(buf->buf);
  }
  
  free(buf);
}

int ep_buffer_read(list_head* head, char* buf, int length) {
  struct ep_buffer* pos = NULL;
  struct ep_buffer* n = NULL;

  int cpy_len = 0;

  list_for_each_entry_safe(pos, n, head, list) {    
    int cur_buf_len = pos->len_total - pos->len_sent;
    if(cur_buf_len <= length - cpy_len) {
      list_del(&pos->list);
      memcpy(buf + cpy_len, pos->buf + pos->len_sent, cur_buf_len);
      ep_buffer_free(pos);
      cpy_len += cur_buf_len;
    } else {
      memcpy(buf + cpy_len, pos->buf + pos->len_sent, length - cpy_len);
      cpy_len += (length - cpy_len);
      pos->len_sent += (length - cpy_len);
      break;
    }
  }
  return cpy_len;

}