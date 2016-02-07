#include "sys_network.h"
#include "ep.h"
#include "ep_poll.h"
#include "ep_poll_epoll.c"
#include "list.h"
#include <unistd.h>
#include <sys/time.h>
#include <stdbool.h>
#define INIT_POOL_SIZE 1024

enum {
  EP_TIMER_FD,
  EP_TIMER_TIMER
};

struct ep_file {
  int fd;
  void* args;
  ep_filecb cb;
  short events;
  struct ep_base* base;
  struct ep_timer_info* timer_info;
  int timeout;
};

struct ep_fd_info {
  short mask;
  struct ep_file* file;
};

struct ep_timer {
  struct ep_base* base;
  struct ep_timer_info* info;
  ep_timercb cb;
  bool pending;
  void* args;
};

struct ep_timer_info {
  list_head list;
  short timer_type;
  void* info;
  int timeout_ms;
  bool pending;
};

struct ep_base {
  struct ep_fd_info* files;
  list_head list_timers;
  struct ep_poll* poll;
  int cur_poll_size;
  int fd_count;
  bool is_stop;
};

static int get_ms() {
  struct timeval val = {};
  gettimeofday(&val, NULL);
  return val.tv_sec * 1000 + val.tv_usec / 1000;
}

static int ep_timer_get_next(struct ep_base* base) {
  int ms = get_ms();
  if(list_empty(&base->list_timers)) {
    return -1;
  }
  list_head* first = base->list_timers.next;
  struct ep_timer_info * tmr = list_entry(first, struct ep_timer_info, list);
  //fprintf(stderr, "eptimer_get_next:%d\n", tmr->timeout_ms - ms);
  return tmr->timeout_ms - ms;
}

static struct ep_timer_info* ep_timer_info_create(void* args, short type) {
  struct ep_timer_info* info = (struct ep_timer_info*) calloc(1, sizeof(struct ep_timer_info));
  info->info = args;
  info->timer_type = type;
  return info;
}

static int ep_timer_info_add(struct ep_base* base, struct ep_timer_info* info, int millsec) {
  
  list_head* pos_add = NULL;
  struct ep_timer_info* pos = NULL;

  if(info->pending) {
    fprintf(stderr, "pending\n");
    return -1;
  }

  info->pending = true;
  
  info->timeout_ms = get_ms() + millsec;
  fprintf(stderr, "ep_timer_info add:%d, timeout ms=%d\n", millsec, info->timeout_ms);
  list_for_each_entry(pos, &base->list_timers, list) {
    if(pos->timeout_ms > millsec) {
      pos_add = pos->list.prev;
    }
  }

  if(!pos_add) {
    fprintf(stderr, "adding tail\n");
    list_add_tail(&info->list, &base->list_timers);
  } else {
    fprintf(stderr, "adding before, ms=%d\n", info->timeout_ms);
    list_add(&info->list, pos_add);
  }
  return 0;
}

static void ep_timer_info_del(struct ep_timer_info* info) {
  if(!info->pending) {
    return;
  }
  info->pending = false;
  list_del(&info->list);
}

static int ep_timer_check(struct ep_base* base) {
  struct ep_timer_info* pos = NULL;
  struct ep_timer_info* temp = NULL;
  int ms = get_ms();
  //fprintf(stderr, "ep_timer_check,curms = %d\n", ms);
  list_for_each_entry_safe(pos, temp, &base->list_timers, list) {
    if(pos->timeout_ms > ms) {
      break;
    }

    ep_timer_info_del(pos);
    pos->pending = false;
    if(pos->timer_type == EP_TIMER_TIMER) {
      struct ep_timer* timer = (struct ep_timer*)pos->info;
      //fprintf(stderr, "active timer,timeout ms = %d\n", pos->timeout_ms);
      timer->cb(timer, timer->args);
    } else {
      //fprintf(stderr, "active file,timeout ms = %d\n", pos->timeout_ms);
      struct ep_file* file = (struct ep_file*)pos->info;
      int fd = file->fd;
      file->cb(file, file->fd, EP_TIMEOUT, file->args);
      file = base->files[fd].file;
      if(file && file->timeout >= 0) {
        if(!file->timer_info) {
          file->timer_info = ep_timer_info_create(file, EP_TIMER_FD);
        }
        ep_timer_info_add(file->base, file->timer_info, file->timeout);
      }
    
    }
    
  }

  if(list_empty(&base->list_timers)) {
    return -1;
  }
  return 0;
}

struct ep_timer* ep_timer_new(struct ep_base* pool, ep_timercb cb, void* args) {
  struct ep_timer* tmr = (struct ep_timer*)calloc(1, sizeof(struct ep_timer));
  tmr->args = args;
  tmr->cb = cb;
  tmr->base = pool;
  
  return tmr;
}

int ep_timer_add(struct ep_timer* timer, int millsec) {
  if(!timer->info) {
    timer->info = ep_timer_info_create(timer, EP_TIMER_TIMER);
  }
  timer->pending = true;
  ep_timer_info_add(timer->base, timer->info, millsec);
  return 0;
}

void ep_timer_del(struct ep_timer* timer) {
  if(timer->info) {
    ep_timer_info_del(timer->info);
    free(timer->info);
    timer->info = NULL;
  }
  timer->pending = false;
}

void ep_timer_free(struct ep_timer* timer) {
  if(timer->info) {
    ep_timer_info_del(timer->info);
    free(timer->info);
    timer->info = NULL;
  }
  free(timer);
}

int ep_file_get_detect(struct ep_file* ep) {
  if(!ep->base) {
    return -1;
  }

  if(ep->base->files[ep->fd].file == 0) {
    return -1;
  }
  return ep->base->files[ep->fd].mask;
}

int ep_file_get_timeout(struct ep_file* ep) {
  if(!ep->base) {
    return -1;
  }

  if(ep->base->files[ep->fd].file == 0) {
    return -1;
  }

  return ep->base->files[ep->fd].file->timeout;
}

void ep_base_stop(struct ep_base* base) {
  base->is_stop = true;
}

bool ep_timer_pending(struct ep_timer* timer) {
  return timer->pending;
}

struct ep_file* ep_file_create(struct ep_base* pool, int fd, ep_filecb cb, void* args) {
  struct ep_file* file = (struct ep_file*)calloc(1, sizeof(struct ep_file));
  file->args = args;
  file->base = pool;
  file->cb = cb;
  file->fd = fd;
  file->timeout = -1;
  pool->files[fd].file = file;
  return file;
}

int ep_file_detect(struct ep_file* ev, short what, int timeout) {
  
  struct ep_base* base = ev->base;
  int fd = ev->fd;
  //ev->timeout = timeout;
  short mask = base->files[fd].mask;

  fprintf(stderr, "ep_file_detect,mask =%d, what=%d\n", what, mask);
  if(!mask && !what) {
    return 0;
  }
  if(!mask && what) {
    mask = what;
    ep_poll_add(base->poll, fd, what);
    base->files[fd].file = ev;
    base->fd_count ++;
  } else if(!what) {
    ep_poll_del(base->poll, fd);
    base->fd_count --;
    mask = what;
    base->files[fd].file = NULL;
  } else {
    ep_poll_mod(base->poll, fd, what);
    mask = what;
  }
  base->files[fd].mask = mask;

  ev->timeout = timeout;
  if(timeout >= 0 && what) {
    if(!ev->timer_info) {
      ev->timer_info = ep_timer_info_create(ev, EP_TIMER_FD);
    }
    ep_timer_info_del(ev->timer_info);
    ep_timer_info_add(ev->base, ev->timer_info, timeout);
    
  } else {
    if(ev->timer_info) {
      ep_timer_info_del(ev->timer_info);
    }    
  }
  return 0;
}

void ep_file_free(struct ep_file* ep) {
  if(ep->timer_info) {
    if(ep->timer_info->pending) {
      list_del(&ep->timer_info->list);
    }
    free(ep->timer_info);
    ep->timer_info = NULL;
  }

  ep_file_detect(ep, 0, -1);
  free(ep);
}


struct ep_base* ep_base_create() {
  struct ep_base* base = (struct ep_base*)calloc(1, sizeof(struct ep_base));
  base->files = (struct ep_fd_info*)calloc(1, sizeof(struct ep_fd_info) * INIT_POOL_SIZE);
  base->cur_poll_size = INIT_POOL_SIZE;
  base->poll = ep_poll_create();
  INIT_LIST_HEAD(&base->list_timers);
  return base;
}

int ep_base_dispatch(struct ep_base* pool) {
  struct ep_fd fds[128];
  int ret = 0;
  int i = 0;

  do {
    int timeout = ep_timer_get_next(pool);

    if(timeout < 0) {
      timeout = -1;
    }

    ret = ep_poll_dispatch(pool->poll, fds, 128, timeout);

    for(i = 0; i < ret; i++) {
      int fd = fds[i].fd;
      int mask = fds[i].event;



      struct ep_file* file = pool->files[fd].file;

      if(!file) {
        ep_poll_del(pool->poll, fd);
        continue;
      }

      fprintf(stderr, "ep_base_dispatch:calling back:%d\n", mask);
      file->cb(file, fd, mask, file->args);
    }

    int timers = ep_timer_check(pool);

    if(pool->is_stop) {
      break;
    }
  } while(1);
  return 0;
}

void ep_base_free(struct ep_base* pool) {
  fprintf(stderr, "ep_base_free\n");
  ep_poll_destroy(pool->poll);
  free(pool->files);
  free(pool);
}
