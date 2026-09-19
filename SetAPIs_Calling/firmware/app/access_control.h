#ifndef G100_ACCESS_CONTROL_H
#define G100_ACCESS_CONTROL_H

#include "lwip/ip_addr.h"
#include <stddef.h>

int g100_source_is_authorized(const ip_addr_t *address);
int g100_write_authorized_sources_json(char *output, size_t capacity);

#endif
