#ifndef G100_BOOT_REQUEST_H
#define G100_BOOT_REQUEST_H
#include "stm32h750xx.h"
/* One software reset may bypass the recovery window. Power/watchdog resets may not. */
#define G100_RUN_APPLICATION 0x47315255u
static inline int g100_backup_access(void) {
    RCC->APB4ENR |= RCC_APB4ENR_RTCAPBEN;
    PWR->CR1 |= PWR_CR1_DBP;
    __DSB();
    for(unsigned i=0;i<1024;++i) if(PWR->CR1 & PWR_CR1_DBP) return 1;
    return 0;
}
static inline int g100_take_application_request(uint32_t cause) {
    if(!g100_backup_access()) return 0;
    int yes=(cause & RCC_RSR_SFTRSTF) && RTC->BKP0R==G100_RUN_APPLICATION;
    RTC->BKP0R=0;
    __DSB();
    return yes && RTC->BKP0R==0;
}
static inline void g100_request_application(void) {
    if(!g100_backup_access()) return;
    RTC->BKP0R=G100_RUN_APPLICATION;
    __DSB();
    if(RTC->BKP0R==G100_RUN_APPLICATION) NVIC_SystemReset();
}
#endif
