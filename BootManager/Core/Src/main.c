/* USER CODE BEGIN Header */
/*
 * Boot Manager (BM), the immutable root of trust - part of the STM32F103RBT6 three-tier secure bootloader.
 * Author: Adham Ehab   Date: 18/08/2026
 */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "crc.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define FBL_BASE     0x08004000U   /* the updatable Flash Bootloader lives here   */
#define FBL_SIZE     0x0000A000U   /* whole FBL region = 40 KB                    */
#define SLOT_B_BASE  0x08015000U   /* staging slot: holds the new FBL during a self-update */
#define FLASH_PAGE   1024U
#define BM_STATE_ADDR  0x0801F000U /* config page where the FBL records its CRC   */
#define BM_STATE_MAGIC 0xB007F00DU
#define BM_FBL_VALID    1U         /* FBL confirmed good (record may be stale)    */
#define BM_FBL_UPDATING 2U         /* self-update in progress -> verify strictly  */
typedef void (*pFunction)(void);
typedef struct { uint32_t magic; uint32_t crc; uint32_t state; } bm_state_t;

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
/* Hardware CRC over a flash region, one zero-extended byte per word.
   Must match the FBL's CRC scheme exactly (same result for the same bytes). */
static uint32_t bm_crc_region(uint32_t addr, uint32_t len)
{
    uint32_t word, crc = 0U;
    __HAL_CRC_DR_RESET(&hcrc);
    for (uint32_t i = 0U; i < len; i++)
    {
        word = *(volatile uint8_t *)(addr + i);
        crc  = HAL_CRC_Accumulate(&hcrc, &word, 1U);
    }
    return crc;
}

/* Finish an FBL self-update that lost power half way.
 *
 * The FBL records the new image's CRC and the UPDATING flag before its updater touches
 * the FBL region, and the new image stays in Slot B until the FBL has booted. So while
 * Slot B still matches that CRC the copy can just be redone from the start, and a second
 * cut during the redo leaves the same situation behind.
 * Returns 1 once the FBL region matches the recorded CRC. */
static int bm_recover_fbl(const bm_state_t *st)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t page_error = 0U;
    int ok = 0;

    if (bm_crc_region(SLOT_B_BASE, FBL_SIZE) != st->crc)
        return 0;                                   /* nothing trustworthy to copy from */

    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = FBL_BASE;
    erase.NbPages     = FBL_SIZE / FLASH_PAGE;

    HAL_FLASH_Unlock();
    if (HAL_FLASHEx_Erase(&erase, &page_error) == HAL_OK)
    {
        ok = 1;
        for (uint32_t i = 0U; i < FBL_SIZE && ok; i += 2U)
        {
            uint16_t hw = *(volatile uint16_t *)(SLOT_B_BASE + i);
            ok = (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, FBL_BASE + i, hw) == HAL_OK);
        }
    }
    HAL_FLASH_Lock();

    return ok && (bm_crc_region(FBL_BASE, FBL_SIZE) == st->crc);
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

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
  MX_CRC_Init();
  /* USER CODE BEGIN 2 */
  /* An app that started the independent watchdog leaves it running across resets with its
     own short timeout (the demo app uses ~2 s), and re-copying the FBL here, or a signature
     check in the FBL, can take longer than that. Stretch it to the maximum (~26 s): still a
     guard against a hang, no longer a reset in the middle of a repair. The settings carry
     over into the FBL and the SBL. Harmless when the watchdog was never started. */
  IWDG->KR  = 0x5555U;   /* unlock PR and RLR                 */
  IWDG->PR  = 0x06U;     /* prescaler /256                    */
  IWDG->RLR = 0x0FFFU;   /* 4095 ticks at ~156 Hz -> ~26 s    */
  IWDG->KR  = 0xAAAAU;   /* reload with the new values        */

  /* Boot Manager: verify the FBL, then hand control to it. */
  const bm_state_t *st = (const bm_state_t *)BM_STATE_ADDR;
  uint32_t fbl_sp = *(volatile uint32_t *)FBL_BASE;
  int sp_ok  = (fbl_sp >= 0x20000000U && fbl_sp <= 0x20005000U); /* FBL vector table sane? */
  int fbl_ok;

  if (st->magic != BM_STATE_MAGIC)
      fbl_ok = sp_ok;                                             /* first boot: sanity gate only  */
  else if (st->state == BM_FBL_UPDATING)
  {
      /* mid-update: fine only if the new image is already in place, otherwise
         (old, half-erased or half-copied FBL) redo the copy from Slot B */
      fbl_ok = (bm_crc_region(FBL_BASE, FBL_SIZE) == st->crc) || bm_recover_fbl(st);
  }
  else /* BM_FBL_VALID */
      fbl_ok = (bm_crc_region(FBL_BASE, FBL_SIZE) == st->crc) || sp_ok; /* trust CRC, tolerate a stale record */

  if (fbl_ok)
  {
      HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);   /* LED SOLID 0.6s = Boot Manager ran */
      HAL_Delay(600);
      HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

      HAL_RCC_DeInit();
      HAL_DeInit();
      SysTick->CTRL = 0; SysTick->LOAD = 0; SysTick->VAL = 0;
      SCB->VTOR = FBL_BASE;
      __set_MSP(*(volatile uint32_t *)FBL_BASE);
      ((pFunction)(*(volatile uint32_t *)(FBL_BASE + 4)))();   /* -> FBL, never returns */
  }

  /* No valid FBL and nothing to recover from -> slow error blink. */
  while (1) { HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin); HAL_Delay(1000); }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
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
