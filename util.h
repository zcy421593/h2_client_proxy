#ifndef UTIL_H
#define UTIL_H
#include <string.h>

#if defined(__GNUC__)
#define PRINTF_FORMAT(format_param, dots_param) \
    __attribute__((format(printf, format_param, dots_param)))
#else
#define PRINTF_FORMAT(format_param, dots_param)
#endif

int server_create_listen_fd(int port);

int make_socket_nonblocking(int fd);

void append_format(char* buf, int len, const char* format, ...) PRINTF_FORMAT(3, 4);

const char* get_status_string(int status_code, int upgrade);

#endif
