#ifndef HEADER_H
#define HEADER_H
#include "list.h"
#include <stdbool.h>
struct field {
  list_head list;
  char* field;
  char* value;
};

struct header {
  char method[10];
  char version[4];
  char* url;
  char* last_field;
  char* last_value;
  bool is_last_value;
  list_head list_headers;
  char** headers;
  bool header_changed;
  int status_code;
  int field_count;
  bool is_upgrade;
};

struct header* header_new(int type);
const char* header_method(struct header* header);
const char* header_url(struct header* header);
const char* header_value(struct header* header, const char* field);
char** header_fields(struct header* header);
int header_status_code(struct header* header);
const char* headr_version(struct header* header);

void headr_set_version(struct header* header, const char* version);
void header_set_status_code(struct header* header, int code);
void header_set_method(struct header* header, const char* method);
void header_set_url(struct header* header, const char* url, int len);
void header_add_pair(struct header* header, const char* field, const char* value);
void header_append_field(struct header* header, const char* field, int len);
void header_append_value(struct header* header, const char* value, int len);
void header_append_complete(struct header* header);
int header_remove_field(struct header* header, const char* field);
int header_to_str(struct header* header, char* buf, int len);
struct header* header_copy(struct header* header);
void header_free(struct header* header);
void header_get_host(struct header* header, char* host, int len);
int header_get_host_port(struct header* header);

enum {
  HEADER_REQUEST, 
  HEADER_RESPONSE
};
#endif