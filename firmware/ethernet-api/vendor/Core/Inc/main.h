/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define uSD_Detect_Pin GPIO_PIN_3
#define uSD_Detect_GPIO_Port GPIOE
#define TOUCH_INT_Pin GPIO_PIN_15
#define TOUCH_INT_GPIO_Port GPIOA
#define TOUCH_RST_Pin GPIO_PIN_5
#define TOUCH_RST_GPIO_Port GPIOD
#define DS1307_SCL_Pin GPIO_PIN_13
#define DS1307_SCL_GPIO_Port GPIOC
#define DS1307_SDA_Pin GPIO_PIN_8
#define DS1307_SDA_GPIO_Port GPIOI
#define LCD_DISP_V_Pin GPIO_PIN_4
#define LCD_DISP_V_GPIO_Port GPIOD
#define LCD_BL_Pin GPIO_PIN_7
#define LCD_BL_GPIO_Port GPIOC
#define ESP_EN_Pin GPIO_PIN_3
#define ESP_EN_GPIO_Port GPIOG
#define ethernet_RST_Pin GPIO_PIN_2
#define ethernet_RST_GPIO_Port GPIOG
#define BUZZER_Pin GPIO_PIN_6
#define BUZZER_GPIO_Port GPIOH
#define RS485_POWER_VALID_Pin GPIO_PIN_0
#define RS485_POWER_VALID_GPIO_Port GPIOA
#define LED_Pin GPIO_PIN_7
#define LED_GPIO_Port GPIOH
#define GSM_PWRKEY_Pin GPIO_PIN_1
#define GSM_PWRKEY_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
