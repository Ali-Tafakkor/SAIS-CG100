#ifndef G100_OTA_UPDATE_H
#define G100_OTA_UPDATE_H
#include <stddef.h>
#include <stdint.h>

/* Returns zero for paths outside this API, otherwise an HTTP status code. */
int g100_ota_handle(const char *method, const char *path, const uint8_t *body, size_t bytes,
                    char *out, size_t capacity);
void g100_ota_process(uint32_t now);
/* Public discovery hints only; fixed padded query prevents UDP amplification. */
int g100_ota_discovery(const uint8_t *query, size_t bytes, char *out, size_t capacity);
#endif
