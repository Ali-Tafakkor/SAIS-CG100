#include "access_control.h"
#include "authorized_sources.h"
#include "rj_security.h"

#include <stdio.h>

typedef struct {
    unsigned char a, b, c, d;
    const char *label;
} AuthorizedSource;

#define G100_SOURCE(a, b, c, d, label) { a, b, c, d, label },
static const AuthorizedSource sources[] = {
    G100_AUTHORIZED_SOURCE_LIST
};
#undef G100_SOURCE

int g100_source_is_authorized(const ip_addr_t *address)
{
    if(RJ_DEVELOPMENT_OPEN) return 1;
    if (address == NULL || !IP_IS_V4(address)) return 0;
    const ip4_addr_t *ipv4 = ip_2_ip4(address);
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        if (ip4_addr1(ipv4) == sources[i].a && ip4_addr2(ipv4) == sources[i].b &&
            ip4_addr3(ipv4) == sources[i].c && ip4_addr4(ipv4) == sources[i].d) {
            return 1;
        }
    }
    return 0;
}

int g100_write_authorized_sources_json(char *output, size_t capacity)
{
    if(RJ_DEVELOPMENT_OPEN) return snprintf(output,capacity,"{\"ok\":true,\"authentication_required\":false,\"source_ip_filter\":false,\"authorized_sources\":[],\"policy\":\"development_open\"}");
    size_t used = 0;
    int length = snprintf(output, capacity, "{\"ok\":true,\"authorized_sources\":[");
    if (length < 0 || (size_t)length >= capacity) return -1;
    used = (size_t)length;

    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        length = snprintf(output + used, capacity - used,
            "%s{\"ip\":\"%u.%u.%u.%u\",\"label\":\"%s\"}",
            i == 0 ? "" : ",", sources[i].a, sources[i].b,
            sources[i].c, sources[i].d, sources[i].label);
        if (length < 0 || (size_t)length >= capacity - used) return -1;
        used += (size_t)length;
    }

    length = snprintf(output + used, capacity - used,
        "],\"policy\":\"exact IPv4 match; edit authorized_sources.h and reflash\"}");
    if (length < 0 || (size_t)length >= capacity - used) return -1;
    return (int)(used + (size_t)length);
}
