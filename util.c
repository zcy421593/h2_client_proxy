#include "util.h"

#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "sys_network.h"
int make_socket_nonblocking(int fd)
{
#ifdef WIN32
  {
    u_long nonblocking = 1;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) == SOCKET_ERROR) {
      return -1;
    }
  }
#else
  {
    int flags;
    if ((flags = fcntl(fd, F_GETFL, NULL)) < 0) {
      return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
      return -1;
    }
  }
#endif
  return 0;
}

void append_format(char* buf, int len, const char* format, ...) {
  char tmp_buf[4096 * 20] = {};
  va_list ap;
  va_start(ap, format);
  vsnprintf(tmp_buf, sizeof(tmp_buf), format, ap);
  va_end(ap);
  strncat(buf, tmp_buf, len);
}

const char* get_status_string(int status_code, int upgrade)
{
  switch(status_code) {
  case 100: return "100 Continue";
  case 101: return "101 Switching Protocols";
  case 200: return (upgrade?"200 Connection established":"200 OK");
  case 201: return "201 Created";
  case 202: return "202 Accepted";
  case 203: return "203 Non-Authoritative Information";
  case 204: return "204 No Content";
  case 205: return "205 Reset Content";
  case 206: return "206 Partial Content";
  case 300: return "300 Multiple Choices";
  case 301: return "301 Moved Permanently";
  case 302: return "302 Found";
  case 303: return "303 See Other";
  case 304: return "304 Not Modified";
  case 305: return "305 Use Proxy";
  case 306: return "306 (Unused)";
  case 307: return "307 Temporary Redirect";
  case 400: return "400 Bad Request";
  case 401: return "401 Unauthorized";
  case 402: return "402 Payment Required";
  case 403: return "403 Forbidden";
  case 404: return "404 Not Found";
  case 405: return "405 Method Not Allowed";
  case 406: return "406 Not Acceptable";
  case 407: return "407 Proxy Authentication Required";
  case 408: return "408 Request Timeout";
  case 409: return "409 Conflict";
  case 410: return "410 Gone";
  case 411: return "411 Length Required";
  case 412: return "412 Precondition Failed";
  case 413: return "413 Request Entity Too Large";
  case 414: return "414 Request-URI Too Long";
  case 415: return "415 Unsupported Media Type";
  case 416: return "416 Requested Range Not Satisfiable";
  case 417: return "417 Expectation Failed";
  case 425: return "425 Bad Host";
  case 500: return "500 Internal Server Error";
  case 501: return "501 Not Implemented";
  case 502: return "502 Bad Gateway";
  case 503: return "503 Service Unavailable";
  case 504: return "504 Gateway Timeout";
  case 505: return "505 HTTP Version Not Supported";
  default: return "";
  }
}

int server_create_listen_fd(int port) {
  int opt_reuse = 1;
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  make_socket_nonblocking(fd);
  struct sockaddr_in addr = {};
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt_reuse, sizeof(int));
  if(bind(fd, (struct sockaddr *) & addr, sizeof(struct sockaddr_in)) != 0) {
    close(fd);
    return -1;
  }

  if(listen(fd, 20) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}