#include "architecture.h"
#include "board_memory.h"
#include "boot_format.h"
#include "board_status.h"
#include "stm32h750xx.h"
#include <stdio.h>
#include <errno.h>
#include <stddef.h>
extern uint8_t _end,_heap_limit,_ebss,_eimage,_stack_bottom;
static uint8_t *heap_end;
static uint32_t heap_peak,selftest_passes,selftest_errors,last_test,last_confirm;

/* Explicit bank-aware bounds: the new MSP is in DTCM, not above the AXI heap. */
void *_sbrk(ptrdiff_t increment) {
    if(!heap_end) heap_end=&_end;
    uintptr_t current=(uintptr_t)heap_end, lower=(uintptr_t)&_end, upper=(uintptr_t)&_heap_limit;
    if((increment>=0 && (uintptr_t)increment>upper-current) ||
       (increment<0 && (uintptr_t)(-(increment+1))+1u>current-lower)) { errno=ENOMEM; return (void *)-1; }
    uint8_t *old=heap_end;
    heap_end+=increment;
    if((uint32_t)(heap_end-&_end)>heap_peak) heap_peak=(uint32_t)(heap_end-&_end);
    return old;
}
void g100_architecture_init(void) {
    /* Stage 0 owns RCC/FMC. An external-RAM Reset_Handler must not call vendor SystemInit. */
    g100_mpu_application();
    g100_cycle_counter_init();
    g100_watchdog_start();
    uint32_t id;
    if(!g100_qspi_init(&id)) { g_board_status.fault=0xA001; return; }
    G100_BOOT_INFO->jedec=id;
    uint32_t *bottom=(uint32_t *)&_stack_bottom;
    uintptr_t sp=__get_MSP();
    while((uintptr_t)bottom+512u<sp) *bottom++=0xA55A3CC3u;
}
void g100_architecture_process(uint32_t now) {
    if(g_board_status.fault || selftest_errors) return; /* Allow hardware watchdog recovery. */
    g100_watchdog_feed();
    if((uint32_t)(now-last_test)>=100u) {
        last_test=now;
        /* Dedicated top 4 KiB scratch, outside all application/linker pools. */
        volatile uint32_t *p=(volatile uint32_t *)0xC0FFF000u;
        for(uint32_t i=0;i<1024;++i) p[i]=(i*0x9E3779B9u)^selftest_passes;
        SCB_CleanInvalidateDCache_by_Addr((uint32_t *)p,4096);
        __DSB();
        for(uint32_t i=0;i<1024;++i) if(p[i]!=((i*0x9E3779B9u)^selftest_passes)) ++selftest_errors;
        ++selftest_passes;
    }
    if(!G100_BOOT_INFO->confirmed && now>=3000u && selftest_passes>=10u &&
       g_board_status.stage==BOARD_STAGE_RUNNING && (uint32_t)(now-last_confirm)>=1000u) {
        last_confirm=now;
        if(!g100_boot_confirm()) g_board_status.fault=0xA002;
    }
}
int g100_architecture_json(char *out,size_t cap) {
    volatile G100BootInfo *i=G100_BOOT_INFO;
    uint32_t free_stack=0;
    const uint32_t *p=(const uint32_t *)&_stack_bottom;
    while((uintptr_t)p<0x20020000u && *p++==0xA55A3CC3u) free_stack+=4;
    return snprintf(out,cap,
        "{\"architecture\":\"nor-sdram-shadow-v1\",\"bootloader\":\"stage0-v1-development\","
        "\"execution_address\":\"0xC0000000\",\"stack_bank\":\"DTCM\",\"cpu_hz\":400000000,"
        "\"qspi_hz\":50000000,\"sdram_hz\":100000000,\"nor_bytes\":16777216,\"sdram_bytes\":16777216,"
        "\"image_bytes\":%lu,\"image_slot_usable_bytes\":%lu,\"bootloader_bytes\":%lu,"
        "\"slot\":%lu,\"generation\":%lu,\"boot_flags\":%lu,\"boot_confirmed\":%s,"
        "\"boot_memory_errors\":%lu,\"boot_cycles\":%lu,\"metadata_sequence\":%lu,"
        "\"integrity\":\"crc32-development-not-authentication\",\"signed_boot\":false,"
        "\"icache_enabled\":%s,\"dcache_enabled\":%s,\"mpu_enabled\":%s,"
        "\"dma_memory_policy\":\"AXI-and-D2-noncacheable\",\"axi_static_bytes\":%lu,"
        "\"c_heap_capacity_bytes\":%lu,\"c_heap_peak_bytes\":%lu,\"stack_budget_bytes\":65536,"
        "\"stack_watermark_free_bytes\":%lu,\"sdram_test_passes\":%lu,\"sdram_test_errors\":%lu,"
        "\"sd_policy\":\"disabled-this-phase\",\"sd_initialized\":false,\"sd_in_use\":false,"
        "\"sdmmc_clock_enabled\":%s,\"capacity_admission\":\"build-and-image-manifest\","
        "\"configuration_storage\":\"volatile_ram\"}",
        (unsigned long)i->image_bytes,(unsigned long)G100_IMAGE_MAX,(unsigned long)i->reserved[0],
        (unsigned long)i->selected_slot,(unsigned long)i->generation,(unsigned long)i->flags,
        i->confirmed?"true":"false",(unsigned long)i->memory_errors,(unsigned long)i->boot_cycles,
        (unsigned long)i->metadata_sequence,(SCB->CCR&SCB_CCR_IC_Msk)?"true":"false",
        (SCB->CCR&SCB_CCR_DC_Msk)?"true":"false",(MPU->CTRL&1u)?"true":"false",
        (unsigned long)((uintptr_t)&_ebss-0x24000100u),
        (unsigned long)(&_heap_limit-&_end),(unsigned long)heap_peak,(unsigned long)free_stack,
        (unsigned long)selftest_passes,(unsigned long)selftest_errors,
        ((RCC->AHB3ENR&RCC_AHB3ENR_SDMMC1EN)||(RCC->AHB2ENR&RCC_AHB2ENR_SDMMC2EN))?"true":"false");
}
