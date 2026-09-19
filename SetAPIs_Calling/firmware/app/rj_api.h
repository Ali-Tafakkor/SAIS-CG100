#ifndef RJ_API_H
#define RJ_API_H
#include <stddef.h>
#include <stdint.h>
#define RJ_BODY_MAX 16384
#define RJ_RESPONSE_MAX 24576
typedef struct { const char *method,*path,*body,*if_match,*if_none_match,*idempotency_key,*local_ip; } RjRequest;
void rj_api_init(void);
void rj_api_process(uint32_t now);
int rj_api_handle(const RjRequest *r,char *out,size_t cap,char *extra,size_t extra_cap);
int rj_discovery_reply(const char *probe,char *out,size_t cap);
#endif
