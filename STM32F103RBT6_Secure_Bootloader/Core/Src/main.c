/* USER CODE BEGIN Header */
/*
 * Flash Bootloader (FBL) - part of the STM32F103RBT6 three-tier secure bootloader.
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
#include "can.h"
#include "crc.h"
#include "i2c.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "bootloader.h"
#include "bl_config.h"
#include "bl_isotp.h"
#include "bl_uds.h"

/* Set either to 1 in a throwaway build to run the matching self-test at boot and
   halt showing the result on LD2 (0 = normal bootloader operation). Run one at a
   time. Result: LD2 solid = pass; otherwise LD2 blinks the diagnostic code N
   times, pauses, and repeats (codes in bl_isotp.h / bl_uds.h). */
#ifndef BL_ISOTP_SELFTEST_ON_BOOT
#define BL_ISOTP_SELFTEST_ON_BOOT   0
#endif
#ifndef BL_UDS_SELFTEST_ON_BOOT
#define BL_UDS_SELFTEST_ON_BOOT     0
#endif

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define APP_ADDRESS   0x0800E000U   /* Slot A (application) base */
typedef void (*pFunction)(void);

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
#if BL_ISOTP_SELFTEST_ON_BOOT || BL_UDS_SELFTEST_ON_BOOT
/* Report a self-test result on LD2 and halt: code 0 = solid ON (pass); any other
   code blinks N times, pauses ~1.5 s, and repeats, so it can be read without a
   debugger. */
static void bl_selftest_report(int code)
{
    if (code == 0)
    {
        HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
        while (1) { /* solid ON = pass */ }
    }
    while (1)
    {
        for (int b = 0; b < code; b++)
        {
            HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
            HAL_Delay(250);
            HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
            HAL_Delay(250);
        }
        HAL_Delay(1500);   /* gap before repeating the count */
    }
}
#endif

/* Boot Manager -> Application handoff (Slot A) */
void BootMgr_JumpToApp(void)
{
    /* Only launch a verified application (metadata magic + whole-image CRC) */
    if (!BootMgr_AppValid())
    {
        return;   /* no valid application -> stay in the bootloader */
    }

    HAL_RCC_DeInit();
    HAL_DeInit();
    /* Stop the SysTick Timer so it doesn't fire interrupts during the jump */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    SCB->VTOR = APP_ADDRESS;                                   /* relocate vector table */
    __set_MSP(*(volatile uint32_t *)APP_ADDRESS);             /* load app stack pointer */
    ((pFunction)(*(volatile uint32_t *)(APP_ADDRESS + 4)))(); /* jump to app reset handler */
}

extern uint32_t _sbl_flash_start, _sbl_ram_start, _sbl_ram_end;

/* Copy the SBL from FLASH into RAM, then execute it from RAM */
void BootMgr_RunSBL(void)
{
    uint32_t *src = &_sbl_flash_start;
    uint32_t *dst = &_sbl_ram_start;
    while (dst < &_sbl_ram_end) { *dst++ = *src++; }   /* stage image into RAM */

    SysTick->CTRL = 0;                       /* stop SysTick (no handler in SBL table) */
    __DSB(); __ISB();
    SCB->VTOR = 0x20001000U;                 /* vector table now in RAM */
    __set_MSP(*(volatile uint32_t *)0x20001000U);
    ((pFunction)(*(volatile uint32_t *)0x20001004U))();  /* jump into RAM */
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  SCB->VTOR = 0x08004000U;   /* FBL lives at 0x08004000 */

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
  MX_CAN_Init();
  MX_CRC_Init();
  MX_USART2_UART_Init();
  MX_USART1_UART_Init();
  MX_I2C1_Init();
  MX_SPI2_Init();
  /* USER CODE BEGIN 2 */
#if BL_ISOTP_SELFTEST_ON_BOOT
  /* Throwaway diagnostic: exercise the isotp-c stack in software loopback.
     Halts showing the result on LD2 (codes documented in bl_isotp.h). */
  bl_selftest_report(BL_ISOTP_SelfTest());
#endif
#if BL_UDS_SELFTEST_ON_BOOT
  /* Throwaway diagnostic: drive a full UDS reprogramming sequence in software
     loopback. Halts showing the result on LD2 (codes documented in bl_uds.h). */
  bl_selftest_report(BL_UDS_SelfTest());
#endif
  FBL_EnsureBmState();   /* keep the Boot Manager.s FBL-CRC record current */

  /* Power-on self-test. RAM + CRC engine are critical: if either fails we can't
     trust the integrity checks the rest of the boot relies on, so halt in a
     fast error blink rather than launch anything. VDD is advisory. The result
     is kept and can be read back with the BIST command. */
  {
      bist_result_t bist;
      if (!BIST_Run(&bist))
      {
          while (1) { HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin); HAL_Delay(60); }
      }
  }

  (void)&BootMgr_RunSBL;   /* kept for later use (SBL runs from RAM) */

  /* Boot decision: if the B1 button (PC13) is NOT held, launch the app.
     BootMgr_JumpToApp returns only if there is no valid app in Slot A. */
  if (HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) == GPIO_PIN_SET)
  {
      /* BootTrial_AllowJump records this boot attempt and returns 0 once a
         freshly installed app has failed to confirm itself too many times ->
         we fall through to bootloader mode (recovery) instead of relaunching it. */
      if (BootTrial_AllowJump())
      {
          BootMgr_JumpToApp();
      }
  }

  /* Button held, or no valid app: show bootloader mode, then wait for commands. */
  for (int i = 0; i < 6; i++)
  {
      HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
      HAL_Delay(80);
  }
  BL_Run();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    HAL_Delay(200);   /* Boot Manager heartbeat blink */
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
