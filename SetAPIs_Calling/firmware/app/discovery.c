#include "discovery.h"
#include "access_control.h"
#include "device_config.h"
#include "device_identity.h"
#include "lwip/netif.h"
#include "main.h"
#include "rj_api.h"
#include "rj_security.h"

#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include <string.h>
extern struct netif gnetif;
static struct udp_pcb *modern;
static struct {
    ip_addr_t source;
    u16_t port;
    uint32_t due;
    int length;
    char body[1201];
} pending;
static struct {
    u32_t ip;
    uint32_t at;
    int used;
} recent[8];
static unsigned recent_index;
static uint32_t global_at;
static unsigned global_count;
static void modern_receive(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *source,
                           u16_t port) {
    (void)arg;
    (void)pcb;
    if (!p)
        return;
    uint32_t now = HAL_GetTick();
    char query[1201];
    if (pending.length || p->tot_len < 512 || p->tot_len > 1200 || !IP_IS_V4(source) ||
        !g100_source_is_authorized(source))
        goto done;
    const ip4_addr_t *s = ip_2_ip4(source);
    if (ip4_addr_isany_val(*s) || ip4_addr_ismulticast(s) || ip4_addr_isbroadcast(s, &gnetif) ||
        !ip4_addr_netcmp(s, netif_ip4_addr(&gnetif), netif_ip4_netmask(&gnetif)))
        goto done;
    if ((uint32_t)(now - global_at) >= 1000) {
        global_at = now;
        global_count = 0;
    }
    if (global_count >= 5)
        goto done;
    for (unsigned i = 0; i < 8; i++)
        if (recent[i].used && recent[i].ip == s->addr && (uint32_t)(now - recent[i].at) < 1000)
            goto done;
    pbuf_copy_partial(p, query, p->tot_len, 0);
    query[p->tot_len] = 0;
    if (memchr(query, 0, p->tot_len))
        goto done;
    int n = rj_discovery_reply(query, pending.body, sizeof(pending.body));
    if (n <= 0 || n > p->tot_len)
        goto done;
    unsigned char jitter;
    if (rj_random(NULL, &jitter, 1))
        jitter = (unsigned char)now;
    pending.source = *source;
    pending.port = port;
    pending.length = n;
    pending.due = now + jitter % 251;
    recent[recent_index].ip = s->addr;
    recent[recent_index].at = now;
    recent[recent_index].used = 1;
    recent_index = (recent_index + 1) % 8;
    global_count++;
done:
    pbuf_free(p);
}
void g100_discovery_process(void) {
    if (pending.length && (int32_t)(HAL_GetTick() - pending.due) >= 0) {
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)pending.length, PBUF_RAM);
        if (p) {
            if (pbuf_take(p, pending.body, (u16_t)pending.length) == ERR_OK)
                (void)udp_sendto(modern, p, &pending.source, pending.port);
            pbuf_free(p);
        }
        pending.length = 0;
    }
}

static void discovery_receive(void *arg, struct udp_pcb *pcb, struct pbuf *packet,
                              const ip_addr_t *source, u16_t source_port) {
    (void)arg;
    const size_t query_length = strlen(G100_DISCOVERY_QUERY);
    char query[sizeof(G100_DISCOVERY_QUERY)];

    if (g100_source_is_authorized(source) && packet->tot_len == query_length) {
        (void)pbuf_copy_partial(packet, query, (u16_t)query_length, 0);
        query[query_length] = '\0';
        if (memcmp(query, G100_DISCOVERY_QUERY, query_length) == 0) {
            char json[640];
            int length = g100_write_status_json(json, sizeof(json));
            if (length > 0 && (size_t)length < sizeof(json)) {
                struct pbuf *reply = pbuf_alloc(PBUF_TRANSPORT, (u16_t)length, PBUF_RAM);
                if (reply != NULL) {
                    if (pbuf_take(reply, json, (u16_t)length) == ERR_OK) {
                        (void)udp_sendto(pcb, reply, source, source_port);
                    }
                    pbuf_free(reply);
                }
            }
        }
    }
    pbuf_free(packet);
}

int g100_discovery_init(void) {
    struct udp_pcb *pcb = udp_new();
    if (pcb == NULL)
        return -1;
    if (udp_bind(pcb, IP_ADDR_ANY, G100_DISCOVERY_PORT) != ERR_OK) {
        udp_remove(pcb);
        return -2;
    }
    udp_recv(pcb, discovery_receive, NULL);
    modern = udp_new();
    if (!modern)
        return -3;
    if (udp_bind(modern, IP_ADDR_ANY, 55670) != ERR_OK) {
        udp_remove(modern);
        modern = NULL;
        return -4;
    }
    udp_recv(modern, modern_receive, NULL);
    return 0;
}
