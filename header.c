
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "header.h"
#include "list.h"
#include "util.h"
#include "http_parser.h"

struct header* header_new(int type) {
  struct header* header = (struct header*)calloc(1, sizeof(struct header));
  INIT_LIST_HEAD(&header->list_headers);
  return header;
}

const char* header_method(struct header* header) {
  return header->method;
}

const char* header_url(struct header* header) {
  return header->url;
}

const char* header_value(struct header* header, const char* field) {
  struct field* pos = NULL;

  list_for_each_entry(pos, &header->list_headers, list) {
    if(strcasecmp(pos->field, field) == 0) {
      return pos->value;
    }
  }

  return NULL;
}

char** header_fields(struct header* header) {
  struct field* pos = NULL;
  if(header->header_changed && header->headers) {
    free(header->headers);
    header->headers = NULL;
  }
  header->header_changed = false;
  header->headers = (char**)calloc(1, header->field_count * 2 + 1);
  char** headers = header->headers;

  list_for_each_entry(pos, &header->list_headers, list) {
    *(headers++) = pos->field;
    *(headers++) = pos->value;
  }
  return header->headers;
}

int header_status_code(struct header* header) {
  return header->status_code;
}

const char* headr_version(struct header* header) {
  return header->version;
}

void headr_set_version(struct header* header, const char* version) {
  strncpy(header->version, version, sizeof(header->version));
}

void header_set_status_code(struct header* header, int code) {
  header->status_code = code;
}

void header_set_method(struct header* header, const char* method) {
  strncpy(header->method, method, sizeof(header->method));
}

void header_set_url(struct header* hdr, const char* url, int len) {
  if(hdr->url) {
    free(hdr->url);
  }
  hdr->url = strndup(url, len);
}

void header_add_pair(struct header* header, const char* field, const char* value) {
  struct field* f = (struct field*)calloc(1, sizeof(struct field));
  f->field = strdup(field);
  f->value = strdup(value);
  header->field_count +=1;
  list_add_tail(&f->list, &header->list_headers);
}

void header_append_field(struct header* header, const char* field, int len) {

  if(header->is_last_value) {
    header_append_complete(header);
  }

  if(header->last_field) {
    char* old_field = header->last_field;
    int new_len = strlen(old_field) + len;
    header->last_field = (char*)calloc(1, new_len + 1);
    strncpy(header->last_field, old_field, new_len);
    memcpy(header->last_field + strlen(old_field), field, len);
    free(old_field);
  } else {
    header->last_field = strndup(field, len);
  }
  header->is_last_value = false;
}

void header_append_value(struct header* header, const char* value, int len) {
  if(header->last_value) {
    char* old_value = header->last_value;
    int new_len = strlen(old_value) + len;
    header->last_value = (char*)calloc(1, new_len + 1);
    strncpy(header->last_value, old_value, new_len);
    memcpy(header->last_value + strlen(old_value), value, len);
    free(old_value);
  } else {
    header->last_value = strndup(value, len);
  }
  header->is_last_value = true;
}

void header_append_complete(struct header* header) {
  if(!header->is_last_value) {
    return;
  }
  assert(header->last_field);
  assert(header->last_value);
  struct field* f = (struct field*)calloc(1, sizeof(struct field));
  f->field = header->last_field;
  f->value = header->last_value;
  header->last_field = NULL;
  header->last_value = NULL;
  header->is_last_value = false;

  list_add_tail(&f->list, &header->list_headers);

}

int header_to_str(struct header* header, char* buf, int len) {
  memset(buf, 0, len);
  struct field* pos = NULL;
  append_format(buf, len, "GET %s HTTP/1.1\r\n", header->url);
  list_for_each_entry(pos, &header->list_headers, list) {
    append_format(buf, len, "%s: %s\r\n", pos->field, pos->value);
  }
  return 0;
}

void header_free(struct header* header) {
  struct field* pos = NULL;
  struct field* n = NULL;
  if(header->url) {
    free(header->url);
  }

  if(header->headers) {
    free(header->headers);
  }
  list_for_each_entry_safe(pos, n, &header->list_headers, list) {
    free(pos->value);
    free(pos->field);
    list_del(&pos->list);
    free(pos);
  }
  free(header);
}

void header_get_host(struct header* header, char* host, int len) {
  struct http_parser_url url;
  http_parser_parse_url_(header->url, strlen(header->url), strcasecmp(header->method, "connect")== 0, &url);
  if(url.field_set & 1 << UF_HOST) {
    strncpy(host, &header->url[url.field_data[UF_HOST].off], url.field_data[UF_HOST].len);
  }
}
int header_get_host_port(struct header* header) {
  struct http_parser_url url;
  http_parser_parse_url_(header->url, strlen(header->url), strcasecmp(header->method, "connect")== 0, &url);
  if(url.field_set & 1 << UF_PORT) {
    return url.port;
  } else {
    return 80;
  }
}