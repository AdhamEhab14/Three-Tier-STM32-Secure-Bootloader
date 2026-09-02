/* USER CODE BEGIN Header */
/*
 * Multi-bus bridge for bl_host.py - part of the STM32F103RBT6 three-tier secure bootloader.
 * Author: Adham Ehab   Date: 18/08/2026
 *
 * One Blue Pill, one USB-serial link to the PC, three downstream buses to the FBL.
 * The FBL listens on all of them at once; the Blue Pill is the bus MASTER, so it has
 * to push each command onto exactly one bus. The PC picks which with a one-time mode
 * byte (bl_host's  can:/spi:/i2c:  prefix), then everything speaks the framed protocol:
 *
 *   PC --UART--> Blue Pill --+--CAN (ISO-TP, PA11/PA12)------> FBL
 *                            +--SPI (PA4-7 + PB0 READY)------> FBL
 *                            +--I2C (PB6/PB7 + PB0 READY)----> FBL
 *
 * SET_BRIDGE (0xE0) is handled locally by the Blue Pill (not forwarded); every other
 * command is relayed on the selected bus and the reply handed back to the PC. SPI and
 * I2C share the FBL's DATA_READY line (PB0) so a slow command can take its time.
 *
 * Wiring (3.3 V, common ground):
 *   SPI - PA5 SCK -> PB13, PA6 MISO <- PB14, PA7 MOSI -> PB15, PA4 NSS -> PB12
 *   I2C - PB6 SCL <-> PB6, PB7 SDA <-> PB7  (4.7k-3.3k pull-ups on both lines)
 *   CAN - PA11/PA12 via an MCP2551, as the original CAN bridge
 *   PB0 READY <- Nucleo PB1 (shared by SPI and I2C)
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "can.h"
#include "i2c.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define CAN_CMD_ID    0x7E0U   /* bridge -> FBL over CAN */
#define CAN_REPLY_ID  0x7E8U   /* FBL -> bridge over CAN */

#define CMD_SET_BRIDGE 0xE0U   /* local command: choose the downstream bus */
#define BUS_CAN 0U
#define BUS_SPI 1U
#define BUS_I2C 2U

/* Set to 1 in a throwaway build to turn the Blue Pill into a UDS client that
   drives the Nucleo's live iso14229 server over CAN (two-board test), instead
   of the normal transparent bridge. 0 = normal bridge. */
#ifndef BP_UDS_CLIENT_ON_BOOT
#define BP_UDS_CLIENT_ON_BOOT 0
#endif

#define I2C_ADDR7  0x42U       /* the FBL's I2C slave address (7-bit) */

/* SPI handshake lines to the FBL slave */
#define NSS_LOW()        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET)
#define NSS_HIGH()       HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET)
#define READY_IS_HIGH()  (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_0) == GPIO_PIN_SET)

#define LED_TOGGLE()     HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static uint8_t bridge_bus = BUS_CAN;   /* default so a plain COM port still bridges CAN */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* ==========================================================================
 *  CAN downstream: ISO-TP (ISO 15765-2) with flow control, sender side.
 * ========================================================================== */
static int bp_send_frame(uint32_t can_id, const uint8_t *bytes, uint8_t length)
{
  CAN_TxHeaderTypeDef header = {0};
  uint32_t mailbox;
  uint32_t timeout = 100000U;

  header.StdId = can_id;  header.IDE = CAN_ID_STD;  header.RTR = CAN_RTR_DATA;  header.DLC = length;
  while (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0U) { if (timeout-- == 0U) return 0; }
  return (HAL_CAN_AddTxMessage(&hcan, &header, (uint8_t *)bytes, &mailbox) == HAL_OK);
}

static int bp_recv_frame(uint8_t *bytes, uint32_t *timeout)
{
  CAN_RxHeaderTypeDef header;
  while (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) == 0U) { if ((*timeout)-- == 0U) return 0; }
  return (HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &header, bytes) == HAL_OK);
}

static int bp_wait_fc(uint32_t timeout, uint8_t *block_size, uint8_t *st_min)
{
  uint8_t frame[8];
  for (;;) {
    uint32_t remaining = timeout;
    if (!bp_recv_frame(frame, &remaining)) return 0;
    if ((frame[0] & 0xF0U) == 0x30U) {
      uint8_t flow_status = frame[0] & 0x0FU;
      if (flow_status == 0x00U) { *block_size = frame[1]; *st_min = frame[2]; return 1; }
      if (flow_status == 0x02U) return 0;
    }
  }
}

static int bp_cantp_send(uint32_t can_id, const uint8_t *data, uint32_t length)
{
  uint8_t  frame[8];
  uint32_t offset, chunk, i;
  uint8_t  seq_num = 1U, block_size = 0U, st_min = 0U, sent_in_block = 0U;

  if (length <= 7U) {
    frame[0] = (uint8_t)length;
    for (i = 0; i < length; i++) frame[1 + i] = data[i];
    return bp_send_frame(can_id, frame, (uint8_t)(1U + length));
  }

  frame[0] = (uint8_t)(0x10U | ((length >> 8) & 0x0FU));
  frame[1] = (uint8_t)(length & 0xFFU);
  for (i = 0; i < 6U; i++) frame[2 + i] = data[i];
  if (!bp_send_frame(can_id, frame, 8U)) return 0;
  if (!bp_wait_fc(2000000U, &block_size, &st_min)) return 0;

  offset = 6U;
  while (offset < length) {
    frame[0] = (uint8_t)(0x20U | (seq_num & 0x0FU));
    chunk = (length - offset > 7U) ? 7U : (length - offset);
    for (i = 0; i < chunk; i++) frame[1 + i] = data[offset + i];
    if (!bp_send_frame(can_id, frame, (uint8_t)(1U + chunk))) return 0;
    offset += chunk; seq_num++;
    if (st_min != 0U && st_min <= 0x7FU) HAL_Delay(st_min);
    if (block_size != 0U) {
      if (++sent_in_block >= block_size) {
        sent_in_block = 0U;
        if (offset < length && !bp_wait_fc(2000000U, &block_size, &st_min)) return 0;
      }
    }
  }
  return 1;
}

static int bp_cantp_recv_sf(uint8_t *data, uint32_t *length, uint32_t timeout_ms)
{
  CAN_RxHeaderTypeDef header;
  uint8_t  frame[8];
  uint32_t start = HAL_GetTick();

  while (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) == 0U) {
    if (HAL_GetTick() - start > timeout_ms) return 0;
  }
  if (HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &header, frame) != HAL_OK) return 0;
  if ((frame[0] & 0xF0U) == 0x00U) {
    uint32_t i, payload_len = frame[0] & 0x0FU;
    for (i = 0; i < payload_len; i++) data[i] = frame[1 + i];
    *length = payload_len;
    return 1;
  }
  return 0;
}

/* Relay one command over CAN, read the single-frame reply. */
static int forward_can(const uint8_t *cmd, uint32_t total, uint8_t *reply, uint32_t *rlen)
{
  CAN_RxHeaderTypeDef rx; uint8_t junk[8];
  while (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) > 0U)   /* drain stale RX */
    HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &rx, junk);
  if (!bp_cantp_send(CAN_CMD_ID, cmd, total)) return 0;
  return bp_cantp_recv_sf(reply, rlen, 12000U);
}

/* ==========================================================================
 *  SPI downstream: master, with the FBL's DATA_READY line pacing the reply.
 * ========================================================================== */
static uint8_t spi_xfer(uint8_t out)
{
  uint8_t in = 0U;
  HAL_SPI_TransmitReceive(&hspi1, &out, &in, 1U, HAL_MAX_DELAY);
  return in;
}

static int forward_spi(const uint8_t *cmd, uint32_t total, uint8_t *reply, uint32_t *rlen)
{
  uint32_t i, guard;

  /* Phase 1: clock out the command. Pause after the first byte so the slave -
     which is polling several transports - notices and enters its read loop. */
  NSS_LOW();
  (void)spi_xfer(cmd[0]);
  HAL_Delay(12);
  for (i = 1U; i < total; i++) (void)spi_xfer(cmd[i]);
  NSS_HIGH();

  /* Phase 2: wait for DATA_READY, then clock the reply back. */
  guard = 0U;
  while (!READY_IS_HIGH()) { HAL_Delay(1); if (++guard > 15000U) return 0; }   /* up to 15 s */

  NSS_LOW();
  reply[0] = spi_xfer(0x00);          /* ACK (0xCD) or NACK (0xAB) */
  *rlen = 1U;
  if (reply[0] == 0xCDU) {
    uint8_t n = spi_xfer(0x00);       /* payload length */
    reply[1] = n; *rlen = 2U;
    for (i = 0U; i < n; i++) { reply[2U + i] = spi_xfer(0x00); (*rlen)++; }
  }
  NSS_HIGH();

  guard = 0U;
  while (READY_IS_HIGH()) { if (++guard > 2000000U) break; }
  return 1;
}

/* ==========================================================================
 *  I2C downstream: HAL master. Write the command, wait for DATA_READY, then read
 *  a fixed 8-byte reply. Clock-stretching on the slave keeps the timing easy.
 * ========================================================================== */
static int forward_i2c(const uint8_t *cmd, uint32_t total, uint8_t *reply, uint32_t *rlen)
{
  uint32_t i, guard;
  uint8_t  buf[8];

  if (HAL_I2C_Master_Transmit(&hi2c1, (uint16_t)(I2C_ADDR7 << 1),
                              (uint8_t *)cmd, (uint16_t)total, 1000U) != HAL_OK)
    return 0;

  guard = 0U;
  while (!READY_IS_HIGH()) { HAL_Delay(1); if (++guard > 15000U) return 0; }   /* up to 15 s */

  if (HAL_I2C_Master_Receive(&hi2c1, (uint16_t)(I2C_ADDR7 << 1), buf, 8U, 1000U) != HAL_OK)
    return 0;

  reply[0] = buf[0];                 /* ACK (0xCD) or NACK (0xAB) */
  if (buf[0] == 0xCDU) {
    uint8_t n = buf[1];
    reply[1] = n; *rlen = 2U;
    for (i = 0U; i < n && (2U + i) < 8U; i++) { reply[2U + i] = buf[2U + i]; (*rlen)++; }
  } else {
    *rlen = 1U;
  }

  guard = 0U;
  while (READY_IS_HIGH()) { if (++guard > 2000000U) break; }
  return 1;
}

#if BP_UDS_CLIENT_ON_BOOT
/* ==========================================================================
 *  UDS client (two-board test): drive the Nucleo's iso14229 server over CAN.
 *  Requests go out on 0x7E0 (multi-frame handled by bp_cantp_send); the server's
 *  single-frame replies come back on 0x7E8. Result on the LED (PC13, active low):
 *  solid ON = full sequence passed; else it blinks the failing stage 2..8.
 * ========================================================================== */
#define BP_LED_ON()   HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET)
#define BP_LED_OFF()  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET)

/* Send a UDS request and read the single-frame reply. Returns reply length,
   or 0 on failure. */
static uint32_t bp_uds_xfer(const uint8_t *req, uint32_t req_len,
                            uint8_t *resp, uint32_t timeout_ms)
{
  uint32_t rlen = 0U;
  if (!bp_cantp_send(CAN_CMD_ID, req, req_len)) return 0U;
  if (!bp_cantp_recv_sf(resp, &rlen, timeout_ms)) return 0U;
  return rlen;
}

/* Show a result code on the LED and halt (0 = solid ON = pass). */
static void bp_uds_report(int code)
{
  if (code == 0) { BP_LED_ON(); while (1) { } }
  while (1)
  {
    for (int b = 0; b < code; b++) { BP_LED_ON(); HAL_Delay(250); BP_LED_OFF(); HAL_Delay(250); }
    HAL_Delay(1500);
  }
}

/* Run the full reprogramming sequence against the server. Never returns. */
static void bp_uds_client(void)
{
  const uint32_t SLOT_B = 0x08015000U;                 /* staging slot base (matches the FBL map) */
  static const uint8_t secret[4] = { 0x19U, 0x84U, 0xC0U, 0xDEU };
  uint8_t  req[16];
  uint8_t  resp[16];
  uint8_t  seed[4], key[4];
  uint32_t n;
  int      i;

  HAL_Delay(300);   /* let the server settle after power-up */

  /* 1) DiagnosticSessionControl -> programming session. */
  req[0] = 0x10U; req[1] = 0x02U;
  n = bp_uds_xfer(req, 2U, resp, 2000U);
  if (n < 2U || resp[0] != 0x50U || resp[1] != 0x02U) bp_uds_report(2);

  /* Wait out the server's ~1 s SecurityAccess boot delay. */
  HAL_Delay(1200);

  /* 2) SecurityAccess requestSeed. */
  req[0] = 0x27U; req[1] = 0x01U;
  n = bp_uds_xfer(req, 2U, resp, 2000U);
  if (n < 6U || resp[0] != 0x67U || resp[1] != 0x01U) bp_uds_report(3);
  seed[0] = resp[2]; seed[1] = resp[3]; seed[2] = resp[4]; seed[3] = resp[5];

  /* 3) SecurityAccess sendKey (key = seed XOR shared secret). */
  for (i = 0; i < 4; i++) key[i] = (uint8_t)(seed[i] ^ secret[i]);
  req[0] = 0x27U; req[1] = 0x02U;
  req[2] = key[0]; req[3] = key[1]; req[4] = key[2]; req[5] = key[3];
  n = bp_uds_xfer(req, 6U, resp, 2000U);
  if (n < 2U || resp[0] != 0x67U || resp[1] != 0x02U) bp_uds_report(4);

  /* 4) RoutineControl start -> erase the staging slot (routineId 0xFF00). */
  req[0] = 0x31U; req[1] = 0x01U; req[2] = 0xFFU; req[3] = 0x00U;
  n = bp_uds_xfer(req, 4U, resp, 5000U);   /* erase can be slow */
  if (n < 2U || resp[0] != 0x71U || resp[1] != 0x01U) bp_uds_report(5);

  /* 5) RequestDownload: 8 bytes at SLOT_B. [0x34][dfi=0][ALFID=0x44][addr:4][size:4] */
  req[0] = 0x34U; req[1] = 0x00U; req[2] = 0x44U;
  req[3] = (uint8_t)(SLOT_B >> 24); req[4] = (uint8_t)(SLOT_B >> 16);
  req[5] = (uint8_t)(SLOT_B >> 8);  req[6] = (uint8_t)(SLOT_B);
  req[7] = 0x00U; req[8] = 0x00U; req[9] = 0x00U; req[10] = 0x08U;
  n = bp_uds_xfer(req, 11U, resp, 2000U);
  if (n < 1U || resp[0] != 0x74U) bp_uds_report(6);

  /* 6) TransferData block #1: [0x36][BSC=1][8 payload bytes]. */
  req[0] = 0x36U; req[1] = 0x01U;
  req[2] = 0xDEU; req[3] = 0xADU; req[4] = 0xBEU; req[5] = 0xEFU;
  req[6] = 0x11U; req[7] = 0x22U; req[8] = 0x33U; req[9] = 0x44U;
  n = bp_uds_xfer(req, 10U, resp, 3000U);
  if (n < 2U || resp[0] != 0x76U || resp[1] != 0x01U) bp_uds_report(7);

  /* 7) RequestTransferExit. */
  req[0] = 0x37U;
  n = bp_uds_xfer(req, 1U, resp, 2000U);
  if (n < 1U || resp[0] != 0x77U) bp_uds_report(8);

  bp_uds_report(0);   /* full UDS reprogramming sequence succeeded over real CAN */
}
#endif /* BP_UDS_CLIENT_ON_BOOT */
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
  MX_CAN_Init();
  MX_I2C1_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  /* CAN: accept every ID, then start */
  CAN_FilterTypeDef filter = {0};
  filter.FilterBank = 0; filter.FilterMode = CAN_FILTERMODE_IDMASK; filter.FilterScale = CAN_FILTERSCALE_32BIT;
  filter.FilterIdHigh = 0; filter.FilterIdLow = 0; filter.FilterMaskIdHigh = 0; filter.FilterMaskIdLow = 0;
  filter.FilterFIFOAssignment = CAN_RX_FIFO0; filter.FilterActivation = ENABLE;
  HAL_CAN_ConfigFilter(&hcan, &filter);
  HAL_CAN_Start(&hcan);

  NSS_HIGH();   /* SPI slave deselected until we drive a transaction */

  for (int i = 0; i < 6; i++) { LED_TOGGLE(); HAL_Delay(100); }   /* boot = alive */

#if BP_UDS_CLIENT_ON_BOOT
  bp_uds_client();   /* two-board UDS test: drive the Nucleo server, never returns */
#endif

  uint8_t frame[200];   /* a command frame from the PC (VERIFY is ~170 bytes) */
  uint8_t reply[64];    /* the FBL's reply: [ACK][len][payload]               */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* 1. Read one framed command from the PC: [LEN][CMD][DATA][CRC32] */
    if (HAL_UART_Receive(&huart1, &frame[0], 1U, HAL_MAX_DELAY) != HAL_OK) continue;
    uint8_t len = frame[0];
    if (len < 5U || (uint32_t)len + 1U > sizeof(frame)) continue;
    if (HAL_UART_Receive(&huart1, &frame[1], len, 300U) != HAL_OK) continue;
    uint32_t total = (uint32_t)len + 1U;

    LED_TOGGLE();

    /* 2a. SET_BRIDGE is ours: pick the downstream bus, ACK locally, don't forward. */
    if (frame[1] == CMD_SET_BRIDGE) {
      bridge_bus = frame[2];
      uint8_t ack[3] = { 0xCDU, 0x01U, bridge_bus };
      HAL_UART_Transmit(&huart1, ack, 3U, HAL_MAX_DELAY);
      continue;
    }

    /* 2b. Everything else: relay on the selected bus, hand the reply back. */
    uint32_t rlen = 0U;
    int ok;
    if      (bridge_bus == BUS_SPI) ok = forward_spi(frame, total, reply, &rlen);
    else if (bridge_bus == BUS_I2C) ok = forward_i2c(frame, total, reply, &rlen);
    else                            ok = forward_can(frame, total, reply, &rlen);
    if (ok) HAL_UART_Transmit(&huart1, reply, rlen, HAL_MAX_DELAY);
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
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
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
  __disable_irq();
  while (1) { }
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
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
