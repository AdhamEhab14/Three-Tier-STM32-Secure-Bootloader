/* USER CODE BEGIN Header */
/*
 * Demo application launched by the bootloader - part of the STM32F103RBT6 three-tier secure bootloader.
 * Author: Adham Ehab   Date: 18/08/2026
 */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  * @author			: Adham Ehab
  * @date			: 18/8/2026
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Boot-trial confirmation - must match the FBL's bootloader.h */
#define BOOT_TRIAL_ADDR    0x0801F400U
#define BOOT_TRIAL_ACTIVE  0xA5A5U

/* Set to 1 to fake a broken app: it never confirms and never kicks the
   watchdog, so it boot-loops and the FBL rolls it back to recovery. */
#define APP_SIMULATE_HANG  0
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* Independent watchdog, driven straight through its registers so the app needs
   no extra HAL module. LSI ~40 kHz / 64 prescaler / 1250 reload ~= 2 s: if the
   main loop ever stops refreshing it, the chip resets. */
static void App_StartWatchdog(void)
{
    IWDG->KR  = 0xCCCCU;   /* start the watchdog                    */
    IWDG->KR  = 0x5555U;   /* unlock write access to PR/RLR         */
    IWDG->PR  = 0x04U;     /* prescaler /64                         */
    IWDG->RLR = 1250U;     /* reload -> ~2 s timeout                */
    IWDG->KR  = 0xAAAAU;   /* refresh                               */
}

static inline void App_KickWatchdog(void)
{
    IWDG->KR = 0xAAAAU;
}

/* Tell the bootloader we booted cleanly by programming the "confirmed"
   half-word. This only clears bits (0xFFFF -> 0x0000), so no page erase is
   needed. A real product would call this after a genuine self-check; here we
   call it once the app has proven it can run stably for a moment. */
static void App_ConfirmBoot(void)
{
    if (*(volatile uint16_t *)BOOT_TRIAL_ADDR != BOOT_TRIAL_ACTIVE) return; /* not on trial */
    if (*(volatile uint16_t *)(BOOT_TRIAL_ADDR + 2U) == 0x0000U)    return; /* already confirmed */

    HAL_FLASH_Unlock();
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, BOOT_TRIAL_ADDR + 2U, 0x0000U);
    HAL_FLASH_Lock();
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  SCB->VTOR = 0x0800E000U;   /* application vector table base (Slot A) */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* USER CODE BEGIN 2 */
  App_StartWatchdog();

#if APP_SIMULATE_HANG
  /* Broken app: hang before confirming. The watchdog resets us ~2 s later,
     the FBL counts the failed boot, and after a few tries it recovers. */
  while (1) { }
#endif

  /* Prove we can run stably for a moment (kicking the dog the whole time),
     then confirm to the bootloader that this app is good. */
  for (int i = 0; i < 5; i++) { App_KickWatchdog(); HAL_Delay(100); }
  App_ConfirmBoot();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    HAL_Delay(150);   /* slow 0.5 Hz blink = Application running */
    App_KickWatchdog();
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
