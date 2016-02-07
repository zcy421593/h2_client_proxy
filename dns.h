#ifndef DNS_H
#define DNS_H

typedef void (*dnscb)(int ret_code, const char** ips, int ip_count, const char* cname, void* args);
int dns_init(struct ep_base* base, const char* ns_server);
struct dns_req* dns_resolve(const char* host,dnscb cb, void* args);
void dns_cancel(struct dns_req* req);
int dns_fini();
#endif

