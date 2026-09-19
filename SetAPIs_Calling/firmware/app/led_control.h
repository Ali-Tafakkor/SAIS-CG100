#ifndef LED_CONTROL_H
#define LED_CONTROL_H
#include <stdint.h>
typedef enum { LED_AUTO, LED_OFF, LED_ON, LED_DANCE } LedMode;
void led_set_mode(LedMode mode, uint32_t period_ms);
void led_update(uint32_t now, uint32_t activity_until);
const char *led_mode_name(void);
uint32_t led_period_ms(void);
uint32_t led_transition_count(void);
#endif
