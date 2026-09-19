#ifndef G100_MAC_H
#define G100_MAC_H

#include <stdint.h>

/* Stable local-unicast address from all 96 UID bits, in word/LSB-first order.
 * A UID suffix can be shared by an entire manufacturing lot. FNV-1a spreads
 * the full UID over the 46 available address bits. This is not an IEEE-issued
 * globally unique address; production provisioning may replace it later.
 */
static inline void g100_mac_from_uid(uint8_t mac[6], const uint32_t uid[3])
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned word = 0; word < 3; ++word) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= (uint8_t)(uid[word] >> shift);
            hash *= UINT64_C(1099511628211);
        }
    }
    for (unsigned i = 0; i < 6; ++i)
        mac[i] = (uint8_t)(hash >> (8 * (5 - i)));
    mac[0] = (uint8_t)((mac[0] & 0xFCu) | 0x02u);
}

#endif
