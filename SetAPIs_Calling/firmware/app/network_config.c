#include "network_config.h"
#include "device_config.h"
#include "device_identity.h"
#include "lwip.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "main.h"
#include <string.h>
#ifdef G100_SHADOW
#include "config_store.h"
#include "rj_json.h"
#endif

extern struct netif gnetif;

typedef enum {
    NETWORK_WAITING_FOR_LINK,
    NETWORK_DHCP,
    NETWORK_DHCP_ASSIGNED,
    NETWORK_STATIC_FALLBACK,
    NETWORK_STATIC_CONFIGURED
} NetworkMode;

static NetworkMode mode = NETWORK_WAITING_FOR_LINK;
static uint32_t dhcp_started_at;
static int previous_link_up;
static int manual_static;
static ip4_addr_t configured_ip, configured_mask, configured_gateway;
static char configured_hostname[64];

static void clear_address(void) {
    ip4_addr_t zero;
    ip4_addr_set_zero(&zero);
    netif_set_addr(&gnetif, &zero, &zero, &zero);
}

static void start_dhcp(uint32_t now) {
    if (manual_static) {
        dhcp_stop(&gnetif);
        netif_set_addr(&gnetif, &configured_ip, &configured_mask, &configured_gateway);
        mode = NETWORK_STATIC_CONFIGURED;
        return;
    }
    clear_address();
    dhcp_stop(&gnetif);
    if (dhcp_start(&gnetif) == ERR_OK) {
        mode = NETWORK_DHCP;
        dhcp_started_at = now;
    } else {
        mode = NETWORK_WAITING_FOR_LINK;
    }
}

static void use_direct_fallback(void) {
    ip4_addr_t address, netmask, gateway;
    dhcp_stop(&gnetif);
    IP4_ADDR(&address, G100_FALLBACK_IP_A, G100_FALLBACK_IP_B, G100_FALLBACK_IP_C,
             g100_device_slot());
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, G100_FALLBACK_IP_A, G100_FALLBACK_IP_B, G100_FALLBACK_IP_C,
             G100_FALLBACK_GATEWAY_D);
    netif_set_addr(&gnetif, &address, &netmask, &gateway);
    mode = NETWORK_STATIC_FALLBACK;
}

void g100_network_init(uint32_t now) {
    strncpy(configured_hostname, g100_device_hostname(), sizeof(configured_hostname) - 1);
    configured_hostname[sizeof(configured_hostname) - 1] = 0;
    netif_set_hostname(&gnetif, configured_hostname);
#ifdef G100_SHADOW
    /* Recovery and application restore the same confirmed management address. */
    static char saved[2048];static RjJson parsed;
    if(g100_config_load(saved,sizeof(saved),NULL,0) && !rj_parse(&parsed,saved)) {
        int net=rj_get(&parsed,0,"network");
        char ip[16]="0.0.0.0",gw[16]="0.0.0.0",host[64],dns1[16]="",dns2[16]="";
        if(net>=0 && rj_string(&parsed,rj_get(&parsed,net,"hostname"),host,sizeof(host))>=0) {
            int dhcp=rj_eq(&parsed,rj_get(&parsed,net,"mode"),"dhcp");
            rj_string(&parsed,rj_get(&parsed,net,"ipv4_address"),ip,sizeof(ip));
            rj_string(&parsed,rj_get(&parsed,net,"gateway"),gw,sizeof(gw));
            int prefix=(int)rj_number(&parsed,rj_get(&parsed,net,"prefix_length"));
            int dns=rj_get(&parsed,net,"dns_servers");
            if(dns>=0 && parsed.t[dns].count>0) {
                int first=dns+1;rj_string(&parsed,first,dns1,sizeof(dns1));
                if(parsed.t[dns].count>1) rj_string(&parsed,parsed.t[first].next,dns2,sizeof(dns2));
            }
            if(dhcp || (prefix>=1 && prefix<=30 && rj_ipv4(ip,NULL) && rj_ipv4(gw,NULL)))
                g100_network_configure(dhcp,ip,prefix,gw,host,dns1,dns2);
        }
    }
#endif
    previous_link_up = netif_is_link_up(&gnetif) ? 1 : 0;
    if (previous_link_up)
        start_dhcp(now);
    else
        clear_address();
}

void g100_network_process(uint32_t now) {
    int link_up = netif_is_link_up(&gnetif) ? 1 : 0;
    if (link_up != previous_link_up) {
        previous_link_up = link_up;
        if (link_up)
            start_dhcp(now);
        else {
            dhcp_stop(&gnetif);
            clear_address();
            mode = NETWORK_WAITING_FOR_LINK;
        }
    }

    if (mode == NETWORK_DHCP) {
        if (dhcp_supplied_address(&gnetif)) {
            mode = NETWORK_DHCP_ASSIGNED;
        } else if ((uint32_t)(now - dhcp_started_at) >= G100_DHCP_TIMEOUT_MS) {
            use_direct_fallback();
        }
    }
}

const char *g100_network_mode(void) {
    switch (mode) {
    case NETWORK_DHCP:
        return "dhcp-waiting";
    case NETWORK_DHCP_ASSIGNED:
        return "dhcp";
    case NETWORK_STATIC_FALLBACK:
        return "static-fallback";
    case NETWORK_STATIC_CONFIGURED:
        return "static";
    default:
        return "waiting-for-link";
    }
}

void g100_network_configure(int dhcp, const char *ip, int prefix, const char *gateway,
                            const char *hostname, const char *dns1, const char *dns2) {
    manual_static = !dhcp;
    if (manual_static) {
        ip4addr_aton(ip, &configured_ip);
        ip4addr_aton(gateway, &configured_gateway);
        configured_mask.addr = lwip_htonl(0xffffffffUL << (32 - prefix));
    }
    strncpy(configured_hostname, hostname, sizeof(configured_hostname) - 1);
    configured_hostname[sizeof(configured_hostname) - 1] = 0;
    ip_addr_t dns;
    ip_addr_set_zero(&dns);
    if (*dns1)
        ipaddr_aton(dns1, &dns);
    dns_setserver(0, &dns);
    ip_addr_set_zero(&dns);
    if (*dns2)
        ipaddr_aton(dns2, &dns);
    dns_setserver(1, &dns);
    if (netif_is_link_up(&gnetif))
        start_dhcp(HAL_GetTick());
}
