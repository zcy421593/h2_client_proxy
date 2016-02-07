#ifndef EP_POOL_H
#define EP_POOL_H

struct ep_fd {
  int fd;
  short event;
};

static struct ep_poll* ep_poll_create();
static int ep_poll_add(struct ep_poll* pool, int fd, short what);
static int ep_poll_mod(struct ep_poll* pool, int fd, short what);
static int ep_poll_del(struct ep_poll* pool, int fd);
static int ep_poll_dispatch(struct ep_poll* pool, struct ep_fd* fds, int count, int timeout);
static void ep_poll_destroy(struct ep_poll* pool);

#endif
