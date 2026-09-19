#ifndef BOARD_STATUS_H
#define BOARD_STATUS_H

#include <stdint.h>

#define BOARD_STATUS_MAGIC 0x45544831u /* "ETH1" */

enum {
    BOARD_STAGE_RESET = 1,
    BOARD_STAGE_HAL_READY = 2,
    BOARD_STAGE_LWIP_READY = 3,
    BOARD_STAGE_HTTP_READY = 4,
    BOARD_STAGE_RUNNING = 5,
    BOARD_STAGE_ERROR = 0xEE
};

typedef struct {
    uint32_t magic;
    uint32_t uptime_ms;
    uint32_t stage;
    uint32_t fault;
    uint32_t link_up;
    uint32_t netif_up;
    uint32_t phy_address;
    uint32_t phy_id1;
    uint32_t phy_id2;
    uint32_t request_count;
    uint32_t led_on;
    uint32_t rx_poll_count;
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t tx_errors;
    uint32_t dma_status;
    uint32_t mac_status;
    uint32_t rmii_refclk_edges;
    uint32_t rmii_crs_dv_edges;
    uint32_t rmii_rxd0_edges;
    uint32_t rmii_rxd1_edges;
    uint32_t rx_ipv4_frames;
    uint32_t rx_arp_frames;
    uint32_t rx_input_errors;
    uint32_t icmp_echo_seen;
    uint32_t icmp_reply_attempts;
    uint32_t icmp_reply_errors;
    uint32_t icmp_length_errors;
    uint32_t icmp_checksum_errors;
    uint32_t icmp_allocation_errors;
    uint32_t icmp_header_errors;
} BoardStatus;

extern volatile BoardStatus g_board_status;

#endif
