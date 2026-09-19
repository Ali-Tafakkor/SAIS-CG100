#ifndef G100_BOARD_MEMORY_H
#define G100_BOARD_MEMORY_H
#include <stdint.h>
int g100_clock_init(void);
void g100_cycle_counter_init(void);
void g100_delay_cycles(uint32_t ticks);
int g100_qspi_init(uint32_t *jedec);
int g100_qspi_map(void);
int g100_qspi_sector_erase(uint32_t offset);
int g100_qspi_program(uint32_t offset, const void *data, uint32_t bytes);
void g100_mpu_boot(void);
void g100_mpu_application(void);
void g100_sdram_init(void);
uint32_t g100_sdram_test(void);
void g100_watchdog_start(void);
void g100_watchdog_feed(void);
#endif
