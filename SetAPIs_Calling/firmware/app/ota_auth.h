#ifndef G100_OTA_AUTH_H
#define G100_OTA_AUTH_H
#include <stdint.h>
#include <stddef.h>
#define G100_PROVISION_OFFSET 0xE0000u
#define G100_PROVISION_MAGIC 0x324B5547u
#define G100_AUTH_BYTES 36u
typedef struct {
    uint32_t magic, format, uid[3];
    uint8_t key[32];
    uint32_t reserved[2], crc;
} G100Provision;
_Static_assert(sizeof(G100Provision)==64, "provision ABI");
int g100_auth_challenge(char nonce_hex[65], uint32_t *sequence);
int g100_auth_request(const char *path,const uint8_t *body,size_t bytes);
int g100_auth_response(const char *body,char signature[65]);
#endif
