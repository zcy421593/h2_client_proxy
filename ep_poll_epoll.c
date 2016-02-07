#include "ep.h"
#include "ep_poll.h"
#include <stdio.h>
#include <sys/epoll.h>
#include <stdlib.h>
#include <unistd.h>
#define MAX_EVENT_COUNT 128


struct ep_poll {
  int epoll_fd;
  struct epoll_event events[MAX_EVENT_COUNT];
};

static struct ep_poll* ep_poll_create() {
  struct ep_poll* poll = (struct ep_poll*)calloc(1, sizeof(struct ep_poll));
  poll->epoll_fd = epoll_create(1024);
  return poll;
}
static int ep_poll_add(struct ep_poll* pool, int fd, short what) {
  fprintf(stderr, "ep_poll_add, what=%d\n", what);
  struct epoll_event ev = {};
  ev.data.fd = fd;
  if(what & EP_READ) {
    ev.events |= EPOLLIN;
  }

  if(what & EP_WRITE) {
    ev.events |= EPOLLOUT;
  }
  epoll_ctl(pool->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
  return 0;
}

static int ep_poll_mod(struct ep_poll* pool, int fd, short what) {
  struct epoll_event ev = {};
  ev.data.fd = fd;
  if(what & EP_READ) {
    ev.events |= EPOLLIN;
  }

  if(what & EP_WRITE) {
    ev.events |= EPOLLOUT;
  }
  epoll_ctl(pool->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
  return 0;
}

static int ep_poll_del(struct ep_poll* pool, int fd) {
  fprintf(stderr, "ep_poll_del\n");

  epoll_ctl(pool->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
  return 0;
}

static int ep_poll_dispatch(struct ep_poll* pool, struct ep_fd* fds, int count, int timeout) {
  int i = 0;
  
  int ret = epoll_wait(pool->epoll_fd, pool->events, 1, timeout);
  fprintf(stderr, "ep_poll_dispatch,timeout = %d", timeout);
  for(i = 0; i < ret; i++) {
    struct epoll_event* ev = &pool->events[i];
    fds[i].fd = ev->data.fd;
    fds[i].event = 0;

    fprintf(stderr, "fd=%d, ev=%d\n", ev->data.fd, ev->events);
    if(ev->events & EPOLLIN) {
      fds[i].event |= EP_READ;
    }
    if(ev->events & EPOLLOUT) {
      fds[i].event |= EP_WRITE;
    }

    if(ev->events & EPOLLHUP || ev->events & EPOLLERR) {
      fprintf(stderr, "ep error detect\n");
      fds[i].event |= EP_ERROR;
    }
  }
  return ret;
}

static void ep_poll_destroy(struct ep_poll* pool) {
  fprintf(stderr, "ep_poll_destroy\n");
  close(pool->epoll_fd);
  free(pool);
}
