#ifndef G100_NETWORK_CONFIG_H
#define G100_NETWORK_CONFIG_H

#include <stdint.h>

void g100_network_init(uint32_t now);
void g100_network_process(uint32_t now);
const char *g100_network_mode(void);
void g100_network_configure(int dhcp,const char *ip,int prefix,const char *gateway,const char *hostname,const char *dns1,const char *dns2);

#endif
