#include "led_control.h"
#include "main.h"
#include "board_status.h"

static LedMode mode = LED_AUTO;
static uint32_t period = 500;
static uint32_t started;
static uint32_t transitions;

void led_update(uint32_t now, uint32_t activity_until)
{
    uint32_t on;
    switch (mode) {
    case LED_ON: on = 1; break;
    case LED_OFF: on = 0; break;
    case LED_DANCE: on = ((uint32_t)(now - started) % period) < period / 2u; break;
    default:
        on = ((int32_t)(activity_until - now) > 0) ||
             (now % (g_board_status.link_up ? 1000u : 500u) < 100u);
        break;
    }
    if (on != g_board_status.led_on) ++transitions;
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    g_board_status.led_on = on;
}

void led_set_mode(LedMode requested, uint32_t period_ms)
{
    mode = requested;
    if (period_ms >= 100u && period_ms <= 10000u) period = period_ms;
    started = HAL_GetTick();
    led_update(started, 0);
}

const char *led_mode_name(void)
{
    static const char *names[] = {"auto", "off", "on", "dance"};
    return names[mode];
}
uint32_t led_period_ms(void) { return mode == LED_DANCE ? period : 0; }
uint32_t led_transition_count(void) { return transitions; }
