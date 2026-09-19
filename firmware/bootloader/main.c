#include "main.h"
#include "board_memory.h"
#include "boot_format.h"
#include <string.h>
extern void g100_jump(uint32_t vector) __attribute__((noreturn));
extern uint8_t _eimage;

void SysTick_Handler(void) { HAL_IncTick(); }
static void recovery(uint32_t error) __attribute__((noreturn));
static void recovery(uint32_t error) {
    G100_BOOT_INFO->error=error;
    G100_BOOT_INFO->stage=0xEE;
    RCC->AHB4ENR|=RCC_AHB4ENR_GPIOHEN;
    GPIOH->MODER=(GPIOH->MODER & ~(3u<<14))|(1u<<14);
    for(;;) {
        g100_watchdog_feed();
        GPIOH->ODR ^= 1u<<7;
        for(volatile uint32_t i=0;i<3000000u;++i) __NOP();
    }
}
void Default_Handler(void) { recovery(0xF000u|__get_IPSR()); }
void Error_Handler(void) { recovery(0xE001u); }

int main(void) {
    volatile G100BootInfo *info=G100_BOOT_INFO;
    memset((void *)info,0,sizeof(*info));
    info->magic=G100_INFO_MAGIC; info->version=1; info->stage=1;
    info->selected_slot=G100_SLOT_NONE;
    info->reset_cause=RCC->RSR;
    RCC->RSR|=RCC_RSR_RMVF;
    HAL_Init();
    if(g100_clock_init()) recovery(0xC101);
    info->cpu_hz=SystemCoreClock;
    g100_cycle_counter_init();
#ifndef G100_RAM_INIT
    g100_watchdog_start();
#endif
    info->stage=2;
    g100_mpu_boot();
    uint32_t jedec;
    if(!g100_qspi_init(&jedec)) recovery(0xC201);
    info->jedec=jedec;
    g100_sdram_init();
    info->stage=3;
    info->memory_errors=g100_sdram_test();
    if(info->memory_errors) recovery(0xC301);
    info->stage=4;
#ifdef G100_RAM_INIT
    info->selected_slot=G100_SLOT_RAM;
    info->flags=G100_BOOT_RAM_TEST;
    __disable_irq();
    SysTick->CTRL=0;
    SCB->ICSR=SCB_ICSR_PENDSTCLR_Msk|SCB_ICSR_PENDSVCLR_Msk;
    __BKPT(0);
    for(;;) __NOP();
#endif
    G100BootMeta meta;
    G100ImageHeader image;
    uint32_t selected=G100_SLOT_NONE;
    int has_meta=g100_meta_read(&meta);
    if(has_meta) {
        info->metadata_sequence=meta.sequence;
        if(meta.trial_slot!=G100_SLOT_NONE && meta.trial_attempts<G100_MAX_TRIAL_ATTEMPTS &&
           g100_image_validate(meta.trial_slot,&image) && image.generation==meta.trial_generation) {
            ++meta.trial_attempts;
            if(g100_meta_write(&meta)) {
                selected=meta.trial_slot;
                info->flags|=G100_BOOT_TRIAL;
                info->metadata_sequence=meta.sequence;
            } else info->flags|=G100_BOOT_META_DEGRADED;
        }
        if(selected==G100_SLOT_NONE && meta.confirmed_slot<=1 &&
           g100_image_validate(meta.confirmed_slot,&image) && image.generation==meta.confirmed_generation) {
            selected=meta.confirmed_slot;
            if(meta.trial_slot!=G100_SLOT_NONE) info->flags|=G100_BOOT_FALLBACK;
        }
    }
    /* If both metadata sectors are lost, prefer the service image over an
     * arbitrary A/B payload whose previous trial outcome is now unknowable. */
    if(!has_meta && g100_image_validate(G100_SLOT_FACTORY,&image)) {
        selected=G100_SLOT_FACTORY;
        info->flags|=G100_BOOT_FALLBACK|G100_BOOT_META_DEGRADED;
    }
    /* Missing/broken metadata: deterministic valid-slot recovery, never unchecked RAM execution. */
    if(selected==G100_SLOT_NONE) {
        for(uint32_t slot=0;slot<=G100_SLOT_FACTORY;++slot) {
            if(has_meta && slot==meta.trial_slot) continue; /* Do not reselect an exhausted/failed trial. */
            if(g100_image_validate(slot,&image)) {
                selected=slot; info->flags|=G100_BOOT_FALLBACK; break;
            }
        }
    }
    if(selected==G100_SLOT_NONE) recovery(0xC401);
    info->selected_slot=selected; info->generation=image.generation;
    info->image_bytes=image.image_bytes; info->image_crc=image.image_crc;
    info->stage=5;
    const void *source=(const void *)(G100_NOR_BASE+g100_slot_offset(selected)+G100_IMAGE_HEADER_BYTES);
    memcpy((void *)G100_SDRAM_BASE,source,image.image_bytes);
    __DSB();
    if(g100_crc32((const void *)G100_SDRAM_BASE,image.image_bytes)!=image.image_crc) recovery(0xC501);
    info->stage=6; info->boot_cycles=DWT->CYCCNT;
    info->reserved[0]=(uint32_t)&_eimage-0x08000000u;
    g100_watchdog_feed();
    __disable_irq();
    SysTick->CTRL=0; SysTick->LOAD=0; SysTick->VAL=0;
    for(unsigned i=0;i<8;++i) { NVIC->ICER[i]=0xFFFFFFFFu; NVIC->ICPR[i]=0xFFFFFFFFu; }
    SCB->ICSR=SCB_ICSR_PENDSTCLR_Msk|SCB_ICSR_PENDSVCLR_Msk;
    SCB->VTOR=G100_SDRAM_BASE;
    __DSB(); __ISB();
    g100_jump(G100_SDRAM_BASE);
}
