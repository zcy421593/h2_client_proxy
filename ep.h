#ifndef EP_H
#define EP_H
#include <stdbool.h>
struct ep_timer;
struct ep_file;
struct ep_base;
typedef void (*ep_filecb)(struct ep_file* ev, int fd, short what, void* args);
typedef void (*ep_timercb)(struct ep_timer* timer, void* args);
struct ep_file* ep_file_create(struct ep_base* pool, int fd, ep_filecb cb, void* args);
int ep_file_detect(struct ep_file* ev, short what, int timeout);
void ep_file_free(struct ep_file* ep);
int ep_file_get_detect(struct ep_file* ep);
int ep_file_get_timeout(struct ep_file* ep);

struct ep_timer* ep_timer_new(struct ep_base* pool, ep_timercb cb, void* args);
int ep_timer_add(struct ep_timer* timer, int millsec);
bool ep_timer_pending(struct ep_timer* timer);
void ep_timer_del(struct ep_timer* timer);
void ep_timer_free(struct ep_timer* timer);

struct ep_base* ep_base_create();
int ep_base_dispatch(struct ep_base* pool);
void ep_base_stop(struct ep_base* base);
void ep_base_free(struct ep_base* pool);

enum {
  EP_READ = 1,
  EP_WRITE = 2,
  EP_TIMEOUT = 4,
  EP_ERROR = 8
};

#endif // EP_H
