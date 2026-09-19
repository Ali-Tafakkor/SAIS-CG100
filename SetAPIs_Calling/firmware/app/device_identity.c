#include "device_identity.h"
#include "device_config.h"
#include "board_status.h"
#include "led_control.h"
#include "lwip.h"
#include "lwip/netif.h"
#include "main.h"
#include "network_config.h"

#include <stdio.h>

extern struct netif gnetif;

static void write_uid(char output[25])
{
    (void)snprintf(output, 25, "%08lX%08lX%08lX",
                   (unsigned long)HAL_GetUIDw0(),
                   (unsigned long)HAL_GetUIDw1(),
                   (unsigned long)HAL_GetUIDw2());
}

static void write_ipv4(char output[16], const ip4_addr_t *address)
{
    (void)snprintf(output, 16, "%u.%u.%u.%u",
                   (unsigned)ip4_addr1(address), (unsigned)ip4_addr2(address),
                   (unsigned)ip4_addr3(address), (unsigned)ip4_addr4(address));
}

static void write_mac(char output[18])
{
    (void)snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                   gnetif.hwaddr[0], gnetif.hwaddr[1], gnetif.hwaddr[2],
                   gnetif.hwaddr[3], gnetif.hwaddr[4], gnetif.hwaddr[5]);
}

const char *g100_device_name(void)
{
    static char name[30];
    if (!name[0]) {
        char uid[25];
        write_uid(uid);
        (void)snprintf(name, sizeof(name), "G100-%s", uid);
    }
    return name;
}

const char *g100_device_hostname(void)
{
    static char hostname[30];
    if (!hostname[0]) {
        char uid[25];
        write_uid(uid);
        (void)snprintf(hostname, sizeof(hostname), "g100-%s", uid);
    }
    return hostname;
}

unsigned g100_device_slot(void)
{
    uint32_t hash = 2166136261u;
    const uint32_t words[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= (uint8_t)(words[i] >> shift);
            hash *= 16777619u;
        }
    /* The legacy slot is only a short hint; the full UID is the identity. */
    return 2u + hash % 253u;
}

const char *g100_firmware_version(void)
{
    return G100_FIRMWARE_VERSION;
}

int g100_write_identity_json(char *output, size_t capacity)
{
    char uid[25], ip[16], netmask[16], gateway[16], mac[18];
    write_uid(uid);
    write_ipv4(ip, netif_ip4_addr(&gnetif));
    write_ipv4(netmask, netif_ip4_netmask(&gnetif));
    write_ipv4(gateway, netif_ip4_gw(&gnetif));
    write_mac(mac);
    return snprintf(output, capacity,
        "{\"ok\":true,\"name\":\"%s\",\"model\":\"%s\","
        "\"firmware\":\"%s\",\"uid\":\"%s\",\"slot\":%u,"
        "\"ip\":\"%s\",\"network_mode\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\","
        "\"mac\":\"%s\",\"http_port\":80,\"discovery\":{"
        "\"protocol\":\"udp-broadcast\",\"port\":%u,"
        "\"query\":\"%s\"}}",
        g100_device_name(), G100_DEVICE_MODEL, G100_FIRMWARE_VERSION, uid,
        g100_device_slot(), ip, g100_network_mode(), netmask, gateway, mac,
        (unsigned)G100_DISCOVERY_PORT, G100_DISCOVERY_QUERY);
}

int g100_write_status_json(char *output, size_t capacity)
{
    char uid[25], ip[16], mac[18];
    write_uid(uid);
    write_ipv4(ip, netif_ip4_addr(&gnetif));
    write_mac(mac);
    led_update(HAL_GetTick(), 0);
    return snprintf(output, capacity,
        "{\"ok\":true,\"name\":\"%s\",\"model\":\"%s\","
        "\"firmware\":\"%s\",\"uid\":\"%s\",\"slot\":%u,"
        "\"ip\":\"%s\",\"network_mode\":\"%s\",\"mac\":\"%s\",\"uptime_ms\":%lu,"
        "\"link_up\":%s,\"netif_up\":%s,\"phy_id\":\"%04lX:%04lX\","
        "\"requests\":%lu,\"rx_frames\":%lu,\"tx_frames\":%lu,"
        "\"tx_errors\":%lu,\"led\":{\"mode\":\"%s\",\"on\":%s,"
        "\"period_ms\":%lu,\"transitions\":%lu}}",
        g100_device_name(), G100_DEVICE_MODEL, G100_FIRMWARE_VERSION, uid,
        g100_device_slot(), ip, g100_network_mode(), mac, (unsigned long)HAL_GetTick(),
        g_board_status.link_up ? "true" : "false",
        g_board_status.netif_up ? "true" : "false",
        (unsigned long)g_board_status.phy_id1, (unsigned long)g_board_status.phy_id2,
        (unsigned long)(g_board_status.request_count + 1u),
        (unsigned long)g_board_status.rx_frames,
        (unsigned long)g_board_status.tx_frames,
        (unsigned long)g_board_status.tx_errors, led_mode_name(),
        g_board_status.led_on ? "true" : "false",
        (unsigned long)led_period_ms(), (unsigned long)led_transition_count());
}
