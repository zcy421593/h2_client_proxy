#include "ep.h"

#include "util.h"
//#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "relay.h"
#include "http_parser.h"
#include "downstream.h"
#include "header.h"
#include "dns.h"
#include "client.h"
#include <signal.h>
#include "sys_network.h"
extern struct upsream_sys http_upsteam;

static int s_listen_fd = 0;
struct ep_base* s_base = NULL;
struct ep_file* s_listen_file = NULL;
struct handler {
  struct downstream* downstream;
  struct upstream* upstream;
  struct header* request_header;
  struct header* response_header;
};

static void* server_do_dispatch(void* args) {
  fprintf(stderr, "server_do_dispatch start\n");
  ep_base_dispatch(s_base);
  dns_fini();

  ep_base_free(s_base);
  fprintf(stderr, "server_do_dispatch complete\n");
  return NULL;
}

static void server_downstreamcb(short what, struct downstream* stream, void* args) {
  if(what == DOWNSTREAM_EVENT_NEW) {

  }
}

static void readcb(struct ep_file* ev, int fd, short what, void* args) {
  struct sockaddr_in addr = {};
  socklen_t len = sizeof(addr);
  int client_fd = accept(fd, (struct sockaddr*)&addr, &len);
  if(client_fd > 0) {
    client_handle_fd(client_fd);
  }
  
}

int server_start() {
  s_base = ep_base_create();
  //http_upsteam.init(s_base, 8123, server_downstreamcb);
  s_listen_fd = server_create_listen_fd(8123);
  s_listen_file = ep_file_create(s_base, s_listen_fd, readcb, NULL);
  ep_file_detect(s_listen_file, EP_READ, -1);
  dns_init(s_base, "114.114.114.114");
  client_init(s_base);
  server_do_dispatch(NULL);
  return 0;
}

extern void h2_server_run(int port);

int main() {
  signal(SIGPIPE,SIG_IGN);
  server_start();
}