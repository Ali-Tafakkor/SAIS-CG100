#ifndef G100_DEVICE_CONFIG_H
#define G100_DEVICE_CONFIG_H

/* Device name, hostname and legacy slot are derived from the silicon UID. */
#define G100_DEVICE_MODEL            "G100"
#ifdef G100_RECOVERY
#define G100_FIRMWARE_VERSION         "1.0.0-recovery"
#elif defined(G100_SHADOW)
#define G100_FIRMWARE_VERSION         "3.3.0-dev-netinstall"
#else
#define G100_FIRMWARE_VERSION         "3.0.0-dev-rj45"
#endif

/*
 * Network policy:
 * - Prefer an address leased by a router through DHCP.
 * - If no DHCP server replies, use the direct-cable fallback below.
 * This keeps the laptop's existing 192.168.2.1/24 Ethernet setting unchanged.
 */
#define G100_DHCP_TIMEOUT_MS          6000u
#define G100_FALLBACK_IP_A            192u
#define G100_FALLBACK_IP_B            168u
#define G100_FALLBACK_IP_C            2u
#define G100_FALLBACK_GATEWAY_D       1u

#define G100_DISCOVERY_PORT           37020u
#define G100_DISCOVERY_QUERY          "G100_DISCOVER_V1"

#endif
