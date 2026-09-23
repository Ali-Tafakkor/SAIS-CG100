#ifndef G100_ARCHITECTURE_H
#define G100_ARCHITECTURE_H
#include <stdint.h>
#include <stddef.h>
void g100_architecture_init(void);
void g100_architecture_process(uint32_t now);
int g100_architecture_json(char *out,size_t cap);
int g100_architecture_ready(void);
#endif
