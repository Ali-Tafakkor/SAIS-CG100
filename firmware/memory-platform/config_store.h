#ifndef G100_CONFIG_STORE_H
#define G100_CONFIG_STORE_H
#include <stddef.h>
#define G100_CONFIG0_OFFSET 0xE1000u
#define G100_CONFIG1_OFFSET 0xE5000u
#define G100_CONFIG_BANK_BYTES 0x4000u
unsigned g100_config_revision(void);
int g100_config_save(const char *settings,const char *instances);
int g100_config_load(char *settings,size_t settings_cap,char *instances,size_t instances_cap);
#endif
