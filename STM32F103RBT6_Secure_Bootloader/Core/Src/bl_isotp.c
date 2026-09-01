/**
 ******************************************************************************
 * @file    bl_isotp.c
 * @author  Adham Ehab
 * @brief   isotp-c (ISO 15765-2) <-> STM32F1 CAN1 glue + loopback self-test.
 *          See bl_isotp.h.
 ******************************************************************************
 */
#include "bl_isotp.h"
#include "can.h"    /* hcan, configured by CubeMX */

/* ==========================================================================
 *  User callbacks required by isotp-c (declared in isotp_user.h).
 *  These are always compiled so the library links; the linker's --gc-sections
 *  drops them together with the rest of isotp-c when nothing calls in.
 * ========================================================================== */

/* Transmit one classical-CAN data frame (<= 8 bytes). */
int isotp_user_send_can(const uint32_t arbitration_id, const uint8_t *data, const uint8_t size)
{
    CAN_TxHeaderTypeDef header = {0};
    uint32_t mailbox;
    uint32_t timeout = 100000U;

    header.StdId = arbitration_id;
    header.IDE   = CAN_ID_STD;
    header.RTR   = CAN_RTR_DATA;
    header.DLC   = size;

    while (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0U) {
        if (timeout-- == 0U) {
            return ISOTP_RET_NOSPACE;   /* isotp-c will retry this frame later */
        }
    }
    if (HAL_CAN_AddTxMessage(&hcan, &header, (uint8_t *)data, &mailbox) != HAL_OK) {
        return ISOTP_RET_ERROR;
    }
    return ISOTP_RET_OK;
}

/* Monotonic microsecond clock. Derived from SysTick (1 ms) - resolution is a
 * millisecond, which is well within ISO-TP's timeout/STmin tolerances here. */
uint32_t isotp_user_get_us(void)
{
    return HAL_GetTick() * 1000U;
}

/* No debug sink in the bootloader; swallow trace output. */
void isotp_user_debug(const char *message, ...)
{
    (void)message;
}

/* ==========================================================================
 *  Link management / RX routing
 * ========================================================================== */

void BL_ISOTP_InitLink(IsoTpLink *link, uint32_t tx_id, uint32_t rx_id,
                       uint8_t *sendbuf, uint32_t sendbufsize,
                       uint8_t *recvbuf, uint32_t recvbufsize)
{
    (void)rx_id;   /* rx_id is used by the pump for routing, not by the link */
    isotp_init_link(link, tx_id, sendbuf, sendbufsize, recvbuf, recvbufsize);
}

void BL_ISOTP_Pump(IsoTpLink **links, const uint32_t *rx_ids, int n)
{
    CAN_RxHeaderTypeDef header;
    uint8_t frame[8];
    int i;

    /* Drain everything currently in the FIFO and route by CAN ID. */
    while (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) > 0U) {
        if (HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &header, frame) != HAL_OK) {
            break;
        }
        if (header.IDE != CAN_ID_STD) {
            continue;   /* this protocol only uses 11-bit IDs */
        }
        for (i = 0; i < n; i++) {
            if (header.StdId == rx_ids[i]) {
                isotp_on_can_message(links[i], frame, (uint8_t)header.DLC);
                break;
            }
        }
    }

    /* Advance each link's transmit/receive state machine. */
    for (i = 0; i < n; i++) {
        isotp_poll(links[i]);
    }
}

/* ==========================================================================
 *  One-board loopback self-test
 * ========================================================================== */

/* Re-init CAN1 into internal loopback with an accept-all filter, so both links
 * below run on a single board with no transceiver (mirrors CAN_BL_Init). */
static void isotp_selftest_can_loopback(void)
{
    CAN_FilterTypeDef filter = {0};

    HAL_CAN_DeInit(&hcan);
    hcan.Init.Mode = CAN_MODE_LOOPBACK;
    HAL_CAN_Init(&hcan);

    filter.FilterBank           = 0;
    filter.FilterMode           = CAN_FILTERMODE_IDMASK;
    filter.FilterScale          = CAN_FILTERSCALE_32BIT;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation     = ENABLE;
    HAL_CAN_ConfigFilter(&hcan, &filter);

    HAL_CAN_Start(&hcan);
}

int BL_ISOTP_SelfTest(void)
{
    /* Two links on one board: "cmd" sends on 0x7E0, "reply" sends on 0x7E8. */
    IsoTpLink cmd_link, reply_link;
    uint8_t cmd_tx[32],   cmd_rx[32];
    uint8_t reply_tx[32], reply_rx[32];

    IsoTpLink *links[2]   = { &cmd_link,        &reply_link };
    uint32_t   rx_ids[2]  = { BL_ISOTP_ID_REPLY, BL_ISOTP_ID_CMD };
    /* cmd_link receives replies (0x7E8); reply_link receives commands (0x7E0). */

    const uint8_t msg[20] = {
        0x10, 0x14, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02,
        0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
        0x0B, 0x0C, 0x0D, 0x0E
    };
    uint8_t  out[32] = {0};
    uint32_t out_len = 0;
    uint32_t guard;
    int i;

    isotp_selftest_can_loopback();

    BL_ISOTP_InitLink(&cmd_link,   BL_ISOTP_ID_CMD,   BL_ISOTP_ID_REPLY,
                      cmd_tx, sizeof(cmd_tx), cmd_rx, sizeof(cmd_rx));
    BL_ISOTP_InitLink(&reply_link, BL_ISOTP_ID_REPLY, BL_ISOTP_ID_CMD,
                      reply_tx, sizeof(reply_tx), reply_rx, sizeof(reply_rx));

    if (isotp_send(&cmd_link, msg, sizeof(msg)) != ISOTP_RET_OK) {
        return 0;
    }

    /* Pump both links until the reply side has the whole message (or we give up). */
    for (guard = 0; guard < 2000000U; guard++) {
        BL_ISOTP_Pump(links, rx_ids, 2);
        if (isotp_receive(&reply_link, out, sizeof(out), &out_len) == ISOTP_RET_OK) {
            break;
        }
    }

    if (out_len != sizeof(msg)) {
        return 0;
    }
    for (i = 0; i < (int)sizeof(msg); i++) {
        if (out[i] != msg[i]) {
            return 0;
        }
    }
    return 1;
}
