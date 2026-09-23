/* MainBoard v2.6 only. No SD, radio, display or fieldbus peripheral is initialized.
 * QSPI/FMC wiring and 400/200/100 MHz timings were measured by memory-probe.
 */
#include "stm32h750xx.h"
#include "board_memory.h"
#include "boot_format.h"
#include "config_store.h"
#define HZ 400000000u
#ifdef G100_SHADOW
#define MPU_CODE __attribute__((section(".itcm_text"),noinline))
#else
#define MPU_CODE
#endif

void g100_cycle_counter_init(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
void g100_delay_cycles(uint32_t ticks) {
    uint32_t start = DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT - start) < ticks) __NOP();
}
static int wait_clear(volatile uint32_t *reg, uint32_t mask, uint32_t ticks) {
    uint32_t start = DWT->CYCCNT;
    while (*reg & mask)
        if ((uint32_t)(DWT->CYCCNT - start) > ticks) return 0;
    return 1;
}
static void pin_af(GPIO_TypeDef *g, unsigned p, unsigned af, unsigned pull) {
    unsigned s = 2u*p, t = 4u*(p & 7u);
    g->OTYPER &= ~(1u << p);
    g->OSPEEDR = (g->OSPEEDR & ~(3u << s)) | (3u << s);
    g->PUPDR = (g->PUPDR & ~(3u << s)) | (pull << s);
    g->AFR[p >> 3] = (g->AFR[p >> 3] & ~(15u << t)) | (af << t);
    g->MODER = (g->MODER & ~(3u << s)) | (2u << s);
}
static int qspi_abort(void) {
    if (QUADSPI->CR & QUADSPI_CR_EN) {
        QUADSPI->CR |= QUADSPI_CR_ABORT;
        if (!wait_clear(&QUADSPI->CR, QUADSPI_CR_ABORT, HZ/4u) ||
            !wait_clear(&QUADSPI->SR, QUADSPI_SR_BUSY, HZ/4u)) return 0;
    }
    QUADSPI->CR = 0;
    QUADSPI->FCR = 0x1Bu;
    return 1;
}
static int command(uint8_t opcode, uint32_t address, int addressed,
                   uint8_t *data, uint32_t bytes, int read) {
    if (!qspi_abort()) return 0;
    QUADSPI->DCR = (23u << 16) | (3u << 8);
    QUADSPI->CR = (19u << 24) | QUADSPI_CR_EN; /* 10 MHz for control/program. */
    QUADSPI->DLR = bytes ? bytes - 1u : 0;
    QUADSPI->CCR = opcode | (1u << 8) |
        (addressed ? ((1u << 10) | (2u << 12)) : 0u) |
        (bytes ? (1u << 24) : 0u) | (read ? (1u << 26) : 0u);
    if (addressed) QUADSPI->AR = address;
    uint32_t start = DWT->CYCCNT;
    for (uint32_t i=0; i<bytes; ++i) {
        if (read) {
            while (!(QUADSPI->SR & QUADSPI_SR_FLEVEL)) {
                if ((QUADSPI->SR & QUADSPI_SR_TEF) ||
                    (uint32_t)(DWT->CYCCNT-start) > HZ/4u) return 0;
            }
            data[i] = *(volatile uint8_t *)&QUADSPI->DR;
        } else {
            while (((QUADSPI->SR >> QUADSPI_SR_FLEVEL_Pos) & 0x3Fu) >= 16u) {
                if ((uint32_t)(DWT->CYCCNT-start) > HZ/4u) return 0;
            }
            *(volatile uint8_t *)&QUADSPI->DR = data[i];
        }
    }
    while (!(QUADSPI->SR & QUADSPI_SR_TCF)) {
        if ((QUADSPI->SR & QUADSPI_SR_TEF) ||
            (uint32_t)(DWT->CYCCNT-start) > HZ/4u) return 0;
    }
    return wait_clear(&QUADSPI->SR, QUADSPI_SR_BUSY, HZ/4u);
}
static int nor_ready(uint32_t ticks) {
    uint32_t start = DWT->CYCCNT;
    uint8_t status = 1;
    do {
        if (!command(0x05, 0, 0, &status, 1, 1)) return 0;
        if (!(status & 1u)) return 1;
    } while ((uint32_t)(DWT->CYCCNT-start) < ticks);
    return 0;
}
static unsigned use_quad;
int g100_qspi_map(void) {
    if (!qspi_abort()) return 0;
    QUADSPI->DCR = (23u << 16) | (3u << 8);
    QUADSPI->CR = (3u << 24) | QUADSPI_CR_SSHIFT | QUADSPI_CR_EN; /* 50 MHz qualified start. */
    QUADSPI->CCR = (use_quad ? 0x6Bu : 0x0Bu) | (1u << 8) | (1u << 10) |
        (2u << 12) | (8u << 18) | ((use_quad ? 3u : 1u) << 24) | (3u << 26);
    __DSB(); __ISB();
    return 1;
}
int g100_qspi_init(uint32_t *jedec) {
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIOEEN | RCC_AHB4ENR_GPIOFEN;
    RCC->AHB3ENR |= RCC_AHB3ENR_QSPIEN;
    (void)RCC->AHB3ENR;
    pin_af(GPIOB,6,10,1); pin_af(GPIOB,2,9,0);
    pin_af(GPIOF,8,10,0); pin_af(GPIOF,9,10,0);
    pin_af(GPIOE,2,9,1); pin_af(GPIOF,6,9,1);
    uint8_t id[3], status;
    /* Survive an MCU-only reset during a previous NOR operation; no chip reset/lock changes. */
    if (!nor_ready(HZ*2u) || !command(0x9F,0,0,id,3,1)) return 0;
    *jedec = ((uint32_t)id[0]<<16) | ((uint32_t)id[1]<<8) | id[2];
    if (*jedec != 0xEF4018u || !command(0x35,0,0,&status,1,1)) return 0;
    use_quad = !!(status & 2u); /* Never modify QE/status/OTP to make a boot succeed. */
    return g100_qspi_map();
}
static int writable_metadata_range(uint32_t off, uint32_t size) {
    return size && ((off >= G100_META0_OFFSET && off+size <= G100_META0_OFFSET+4096u) ||
                    (off >= G100_META1_OFFSET && off+size <= G100_META1_OFFSET+4096u));
}
static int writable_image_range(uint32_t off, uint32_t size) {
    return size && ((off >= G100_SLOT_A_OFFSET && off < G100_SLOT_A_OFFSET+G100_IMAGE_SLOT_BYTES &&
                     size <= G100_SLOT_A_OFFSET+G100_IMAGE_SLOT_BYTES-off) ||
                    (off >= G100_SLOT_B_OFFSET && off < G100_SLOT_B_OFFSET+G100_IMAGE_SLOT_BYTES &&
                     size <= G100_SLOT_B_OFFSET+G100_IMAGE_SLOT_BYTES-off));
}
static int erase_sector(uint32_t off) {
    int ok = nor_ready(HZ*2u) && command(0x06,0,0,0,0,0) &&
             command(0x20,off,1,0,0,0) && nor_ready(HZ*2u);
    int mapped = g100_qspi_map();
    return ok && mapped;
}
int g100_qspi_sector_erase(uint32_t off) {
    return !(off & 4095u) && writable_metadata_range(off,4096) && erase_sector(off);
}
int g100_qspi_image_sector_erase(uint32_t off) {
    return !(off & 4095u) && writable_image_range(off,4096) && erase_sector(off);
}
static int program_page(uint32_t off, const void *data, uint32_t bytes) {
    int ok = nor_ready(HZ) && command(0x06,0,0,0,0,0) &&
             command(0x02,off,1,(uint8_t *)data,bytes,0) && nor_ready(HZ);
    int mapped = g100_qspi_map();
    return ok && mapped;
}
int g100_qspi_program(uint32_t off, const void *data, uint32_t bytes) {
    return data && bytes && bytes <= 256u && (off & 255u)+bytes <= 256u &&
           writable_metadata_range(off,bytes) && program_page(off,data,bytes);
}
int g100_qspi_image_program(uint32_t off, const void *data, uint32_t bytes) {
    return data && bytes && bytes <= 256u && (off & 255u)+bytes <= 256u &&
           writable_image_range(off,bytes) && program_page(off,data,bytes);
}
static int config_range(uint32_t off,uint32_t bytes) {
    return bytes && ((off>=G100_CONFIG0_OFFSET && off<G100_CONFIG0_OFFSET+G100_CONFIG_BANK_BYTES &&
                     bytes<=G100_CONFIG0_OFFSET+G100_CONFIG_BANK_BYTES-off) ||
                    (off>=G100_CONFIG1_OFFSET && off<G100_CONFIG1_OFFSET+G100_CONFIG_BANK_BYTES &&
                     bytes<=G100_CONFIG1_OFFSET+G100_CONFIG_BANK_BYTES-off));
}
int g100_qspi_config_sector_erase(uint32_t off) {
    return !(off&4095u) && config_range(off,4096) && erase_sector(off);
}
int g100_qspi_config_program(uint32_t off,const void *data,uint32_t bytes) {
    return data && bytes && bytes<=256u && (off&255u)+bytes<=256u &&
           config_range(off,bytes) && program_page(off,data,bytes);
}
static inline __attribute__((always_inline)) void region(unsigned n, uint32_t base, unsigned size_log2, uint32_t attributes) {
    MPU->RNR=n; MPU->RBAR=base; MPU->RASR=attributes | ((size_log2-1u)<<1) | 1u;
}
MPU_CODE void g100_mpu_boot(void) {
    __DMB(); MPU->CTRL=0;
    unsigned count=(MPU->TYPE>>8)&0xFFu;
    for(unsigned i=0;i<count;++i) { MPU->RNR=i; MPU->RASR=0; }
    /* SDRAM defaults to Device at C0000000; explicitly Normal, RW executable during load. */
    region(0,G100_SDRAM_BASE,24,(3u<<24)|(1u<<19));
    /* NOR has no executable code; Normal noncacheable to observe program/erase accurately. */
    region(1,G100_NOR_BASE,24,(3u<<24)|(1u<<19)|(1u<<28));
    MPU->CTRL=MPU_CTRL_PRIVDEFENA_Msk|MPU_CTRL_ENABLE_Msk;
    __DSB(); __ISB();
}
MPU_CODE void g100_mpu_application(void) {
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    if (SCB->CCR & SCB_CCR_DC_Msk) SCB_DisableDCache();
    if (SCB->CCR & SCB_CCR_IC_Msk) SCB_DisableICache();
    g100_mpu_boot();
    MPU->CTRL=0;
    /* Whole SDRAM: data RW/XN WB/WA; first 4 MiB code/const: RO executable. */
    region(0,G100_SDRAM_BASE,24,(3u<<24)|(1u<<19)|(1u<<17)|(1u<<16)|(1u<<28));
    region(2,G100_SDRAM_BASE,22,(6u<<24)|(1u<<19)|(1u<<17)|(1u<<16));
    /* Current lwIP TX payloads live in AXI. Keep ALL AXI + D2 noncacheable, not only descriptors. */
    region(3,0x24000000u,19,(3u<<24)|(1u<<19)|(1u<<18)|(1u<<28));
    region(4,0x30000000u,19,(3u<<24)|(1u<<19)|(1u<<18)|(1u<<28));
    MPU->CTRL=MPU_CTRL_PRIVDEFENA_Msk|MPU_CTRL_ENABLE_Msk;
    __DSB(); __ISB();
    SCB_EnableICache(); SCB_EnableDCache();
    __set_PRIMASK(mask);
}
static void sdram_command(uint32_t c) {
    FMC_Bank5_6_R->SDCMR=c|0x10u;
    __DSB(); g100_delay_cycles(HZ/1000000u);
}
void g100_sdram_init(void) {
    RCC->AHB4ENR |= 0x1F8u; /* D/E/F/G/H/I only. */
    RCC->AHB3ENR |= RCC_AHB3ENR_FMCEN;
    (void)RCC->AHB3ENR;
    GPIO_TypeDef *ports[]={GPIOD,GPIOE,GPIOF,GPIOG,GPIOH,GPIOI};
    const unsigned masks[]={0xC703,0xFF83,0xF83F,0x8133,0xFF2C,0x06FF};
    for(unsigned i=0;i<6;++i) for(unsigned p=0;p<16;++p)
        if(masks[i]&(1u<<p)) pin_af(ports[i],p,12,0);
    FMC_Bank1_R->BTCR[0] |= 0x80000000u;
    FMC_Bank5_6_R->SDCR[0]=0x39E4u;
    FMC_Bank5_6_R->SDTR[0]=0x01126461u;
    sdram_command(1); g100_delay_cycles(HZ/1000u);
    sdram_command(2); sdram_command(3|(7u<<5));
    sdram_command(4|(0x230u<<9));
    FMC_Bank5_6_R->SDRTR=1542u<<1;
    __DSB(); g100_delay_cycles(HZ/1000u);
}
uint32_t g100_sdram_test(void) {
    volatile uint32_t *p=(volatile uint32_t *)G100_SDRAM_BASE;
    uint32_t errors=0;
    for(unsigned pass=0;pass<2;++pass) {
        for(uint32_t i=0;i<G100_SDRAM_BYTES/4u;++i) {
            p[i]=((i*0x9E3779B9u)^0xA5A55A5Au) ^ (pass ? 0xFFFFFFFFu:0u);
            if(!(i&0x3FFFu)) g100_watchdog_feed();
        }
        __DSB(); g100_delay_cycles(HZ/5u);
        for(uint32_t i=0;i<G100_SDRAM_BYTES/4u;++i) {
            uint32_t expected=((i*0x9E3779B9u)^0xA5A55A5Au) ^ (pass ? 0xFFFFFFFFu:0u);
            if(p[i]!=expected) ++errors;
            if(!(i&0x3FFFu)) g100_watchdog_feed();
        }
    }
    return errors;
}
void g100_watchdog_feed(void) { IWDG1->KR=0xAAAAu; }
void g100_watchdog_start(void) {
    DBGMCU->APB4FZ1 &= ~DBGMCU_APB4FZ1_DBG_IWDG1;
    IWDG1->KR=0xCCCCu; IWDG1->KR=0x5555u;
    IWDG1->PR=6; IWDG1->RLR=4095;
    /* ~32 seconds at nominal LSI. Reset recovery never depends on a network peer. */
    wait_clear(&IWDG1->SR,7u,HZ/4u);
    g100_watchdog_feed();
}
