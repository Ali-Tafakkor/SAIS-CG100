#include "main.h"
#include "board_status.h"
#include "http_api.h"
#include "led_control.h"
#include "discovery.h"
#include "network_config.h"
#include "rj_api.h"
#include "lwip.h"
#include "ethernetif.h"
#include "lan8742.h"
#include "lwip/etharp.h"

#include <stdint.h>
#ifdef G100_SHADOW
#include "architecture.h"
#include "ota_update.h"
#endif

extern ETH_HandleTypeDef heth;
extern lan8742_Object_t LAN8742;
extern struct netif gnetif;

volatile BoardStatus g_board_status __attribute__((section(".status"), used));

static void board_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();

    HAL_GPIO_WritePin(ethernet_RST_GPIO_Port, ethernet_RST_Pin, GPIO_PIN_RESET);
    gpio.Pin = ethernet_RST_Pin;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(ethernet_RST_GPIO_Port, &gpio);

    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
    gpio.Pin = LED_Pin;
    HAL_GPIO_Init(LED_GPIO_Port, &gpio);
}

#ifndef G100_SHADOW
static int system_clock_init(void)
{
    RCC_OscInitTypeDef oscillator = {0};
    RCC_ClkInitTypeDef clocks = {0};

    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    uint32_t start = HAL_GetTick();
    while (__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY) == 0u) {
        if ((uint32_t)(HAL_GetTick() - start) > 100u) return -1;
    }

    /* External 10 MHz oscillator: / 5 * 400 / 2 = 400 MHz CPU. */
    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    oscillator.HSEState = RCC_HSE_BYPASS;
    oscillator.PLL.PLLState = RCC_PLL_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    oscillator.PLL.PLLM = 5;
    oscillator.PLL.PLLN = 400;
    oscillator.PLL.PLLP = 2;
    oscillator.PLL.PLLQ = 4;
    oscillator.PLL.PLLR = 2;
    oscillator.PLL.PLLRGE = RCC_PLL1VCIRANGE_1;
    oscillator.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
    oscillator.PLL.PLLFRACN = 0;
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) return -2;

    clocks.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                       RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                       RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
    clocks.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clocks.SYSCLKDivider = RCC_SYSCLK_DIV1;
    clocks.AHBCLKDivider = RCC_HCLK_DIV2;
    clocks.APB3CLKDivider = RCC_APB3_DIV2;
    clocks.APB1CLKDivider = RCC_APB1_DIV2;
    clocks.APB2CLKDivider = RCC_APB2_DIV2;
    clocks.APB4CLKDivider = RCC_APB4_DIV2;
    if (HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_2) != HAL_OK) return -3;
    return 0;
}

#endif
static int phy_reference_clock_init(void)
{
    /* PC9/MCO2 supplies PHY XI: HSE 10 MHz / 5 * 200 / 16 = 25 MHz. */
    __HAL_RCC_PLL2_DISABLE();
    uint32_t start = HAL_GetTick();
    while (__HAL_RCC_GET_FLAG(RCC_FLAG_PLL2RDY) != 0u) {
        if ((uint32_t)(HAL_GetTick() - start) > 10u) {
            return -1;
        }
    }
    __HAL_RCC_PLL2_CONFIG(5, 200, 16, 2, 2);
    __HAL_RCC_PLL2_VCIRANGE(RCC_PLL2VCIRANGE_1);
    __HAL_RCC_PLL2_VCORANGE(RCC_PLL2VCOWIDE);
    __HAL_RCC_PLL2FRACN_CONFIG(0);
    __HAL_RCC_PLL2FRACN_DISABLE();
    __HAL_RCC_PLL2_ENABLE();
    start = HAL_GetTick();
    while (__HAL_RCC_GET_FLAG(RCC_FLAG_PLL2RDY) == 0u) {
        if ((uint32_t)(HAL_GetTick() - start) > 10u) {
            return -2;
        }
    }
    __HAL_RCC_PLL2CLKOUT_ENABLE(RCC_PLL2_DIVP);
    HAL_RCC_MCOConfig(RCC_MCO2, RCC_MCO2SOURCE_PLL2PCLK, RCC_MCODIV_1);
    return 0;
}

static void update_diagnostics(void)
{
    uint32_t value = 0;
    g_board_status.uptime_ms = HAL_GetTick();
    g_board_status.link_up = netif_is_link_up(&gnetif) ? 1u : 0u;
    g_board_status.netif_up = netif_is_up(&gnetif) ? 1u : 0u;
    g_board_status.phy_address = LAN8742.DevAddr;
    g_board_status.dma_status = ETH->DMACSR;
    g_board_status.mac_status = ETH->MACISR;
    if (LAN8742.DevAddr <= 31u) {
        if (HAL_ETH_ReadPHYRegister(&heth, LAN8742.DevAddr, LAN8742_PHYI1R, &value) == HAL_OK) {
            g_board_status.phy_id1 = value & 0xFFFFu;
        }
        if (HAL_ETH_ReadPHYRegister(&heth, LAN8742.DevAddr, LAN8742_PHYI2R, &value) == HAL_OK) {
            g_board_status.phy_id2 = value & 0xFFFFu;
        }
    }
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void Default_Handler(void)
{
    g_board_status.fault = 0xF0000000u | (__get_IPSR() & 0x1FFu);
    g_board_status.stage = BOARD_STAGE_ERROR;
    __disable_irq();
    for (;;) {}
}

void Error_Handler(void)
{
    if (g_board_status.fault == 0u) {
        g_board_status.fault = 0xE7000000u |
                               ((g_board_status.stage & 0xFFu) << 16) |
                               (heth.ErrorCode & 0xFFFFu);
    }
    g_board_status.stage = BOARD_STAGE_ERROR;
    for (;;) {
        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
        for (volatile uint32_t delay = 0; delay < 1000000u; ++delay) {
            __NOP();
        }
    }
}

int main(void)
{
    volatile uint32_t *status_word = (volatile uint32_t *)&g_board_status;
    for (uint32_t i = 0; i < sizeof(g_board_status) / sizeof(uint32_t); ++i) {
        status_word[i] = 0u;
    }
    g_board_status.magic = BOARD_STATUS_MAGIC;
    g_board_status.stage = BOARD_STAGE_RESET;

    /* Start on HSI, then use the verified external 10 MHz oscillator. */
    HAL_Init();
    g_board_status.stage = BOARD_STAGE_HAL_READY;
#ifdef G100_SHADOW
    g100_architecture_init();
    if (g_board_status.fault != 0u) Error_Handler();
#else
    if (system_clock_init() != 0) {
        g_board_status.fault = 0x434C4B31u; /* CLK1 */
        Error_Handler();
    }
#endif
    board_gpio_init();
    if (phy_reference_clock_init() != 0) {
        g_board_status.fault = 0x434C4B32u; /* CLK2 */
        Error_Handler();
    }

    MX_LWIP_Init();
    g100_network_init(HAL_GetTick());
    g_board_status.stage = BOARD_STAGE_LWIP_READY;
    update_diagnostics();

    rj_api_init();
    if (http_api_init() != 0) {
        g_board_status.fault = 0x48545450u; /* HTTP */
        Error_Handler();
    }
    if (g100_discovery_init() != 0) {
        g_board_status.fault = 0x55445031u; /* UDP1 */
        Error_Handler();
    }
    g_board_status.stage = BOARD_STAGE_HTTP_READY;

    uint32_t last_diag = 0;
    uint32_t last_request_count = 0;
    uint32_t request_flash_until = 0;
    uint32_t previous_a = GPIOA->IDR;
    uint32_t previous_c = GPIOC->IDR;
    for (;;) {
        uint32_t current_a = GPIOA->IDR;
        uint32_t current_c = GPIOC->IDR;
        uint32_t changed_a = current_a ^ previous_a;
        uint32_t changed_c = current_c ^ previous_c;
        if (changed_a & GPIO_PIN_1) g_board_status.rmii_refclk_edges++;
        if (changed_a & GPIO_PIN_7) g_board_status.rmii_crs_dv_edges++;
        if (changed_c & GPIO_PIN_4) g_board_status.rmii_rxd0_edges++;
        if (changed_c & GPIO_PIN_5) g_board_status.rmii_rxd1_edges++;
        previous_a = current_a;
        previous_c = current_c;
        MX_LWIP_Process();
        uint32_t now = HAL_GetTick();
        g100_network_process(now);
        rj_api_process(now);
        g100_discovery_process();
        if ((uint32_t)(now - last_diag) >= 100u) {
            last_diag = now;
            update_diagnostics();
            g_board_status.stage = BOARD_STAGE_RUNNING;
        }
        if (g_board_status.request_count != last_request_count) {
            last_request_count = g_board_status.request_count;
            request_flash_until = now + 180u;
        }

        led_update(now, request_flash_until);
#ifdef G100_SHADOW
        g100_architecture_process(now);
        g100_ota_process(now);
#endif
    }
}
