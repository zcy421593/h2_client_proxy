#include "list.h"
struct ep_buffer {
  list_head list;
  char* buf;
  int len_total;
  int len_sent;
};

struct ep_buffer* ep_buffer_create(char* buf, int len);
void ep_buffer_free(struct ep_buffer* buf);
int ep_buffer_read(list_head* head, char* buf, int len);