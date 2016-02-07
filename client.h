#ifndef CLIENT_H
#define CLIENT_H
struct ep_base;
int client_init(struct ep_base* base);
void client_handle_fd(int fd);

#endif
