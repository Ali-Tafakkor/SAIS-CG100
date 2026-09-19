#ifndef G100_DEVICE_IDENTITY_H
#define G100_DEVICE_IDENTITY_H

#include <stddef.h>

const char *g100_device_name(void);
const char *g100_device_hostname(void);
unsigned g100_device_slot(void);
const char *g100_firmware_version(void);
int g100_write_identity_json(char *output, size_t capacity);
int g100_write_status_json(char *output, size_t capacity);

#endif
