/**
 ******************************************************************************
 * @file    bl_isotp.c
 * @author  Adham Ehab
 * @brief   isotp-c (ISO 15765-2) <-> STM32F1 CAN1 glue, plus a software-loopback
 *          self-test of the ISO-TP logic. See bl_isotp.h.
 ******************************************************************************
 */
#include "bl_isotp.h"
#include "can.h"    /* hcan, configured by CubeMX */
#include <string.h>

/* ==========================================================================
 *  Software-loopback plumbing (used only by the self-test).
 *
 *  When g_swloop_active is set, isotp_user_send_can queues frames into a small
 *  RAM ring instead of putting them on CAN. The self-test then drains the ring
 *  and feeds each frame to the addressed link. This validates the isotp-c
 *  segmentation / flow-control / reassembly logic and this file's routing with
 *  no dependence on the CAN peripheral (whose single-board loopback bring-up is
 *  a separate matter, validated on the real two-node bus instead).
 * ========================================================================== */

typedef struct {
    uint32_t id;
    uint8_t  data[8];
    uint8_t  len;
} swframe_t;

#define SWQ_LEN 16
static swframe_t g_swq[SWQ_LEN];
static uint16_t  g_swq_head;
static uint16_t  g_swq_tail;
static int       g_swloop_active;

static void swq_push(uint32_t id, const uint8_t *data, uint8_t len)
{
    uint16_t next = (uint16_t)((g_swq_head + 1U) % SWQ_LEN);
    if (next == g_swq_tail) {
        return;   /* full - dropped (should not happen with lockstep sizes) */
    }
    g_swq[g_swq_head].id  = id;
    g_swq[g_swq_head].len = len;
    if (len > 8U) { len = 8U; }
    memcpy(g_swq[g_swq_head].data, data, len);
    g_swq_head = next;
}

static int swq_pop(swframe_t *out)
{
    if (g_swq_tail == g_swq_head) {
        return 0;
    }
    *out = g_swq[g_swq_tail];
    g_swq_tail = (uint16_t)((g_swq_tail + 1U) % SWQ_LEN);
    return 1;
}

/* ==========================================================================
 *  User callbacks required by isotp-c (declared in isotp_user.h).
 *  Always compiled so the library links; the linker's --gc-sections drops them
 *  together with the rest of isotp-c when nothing calls in.
 * ========================================================================== */

/* Transmit one classical-CAN data frame (<= 8 bytes), or queue it into the
   software-loopback ring while the self-test is running. */
int isotp_user_send_can(const uint32_t arbitration_id, const uint8_t *data, const uint8_t size)
{
    CAN_TxHeaderTypeDef header = {0};
    uint32_t mailbox;
    uint32_t timeout = 100000U;

    if (g_swloop_active) {
        swq_push(arbitration_id, data, size);
        return ISOTP_RET_OK;
    }

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
 *  Link management / RX routing (real CAN bus)
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
 *  Software-loopback self-test
 * ========================================================================== */

/* Drain the software ring into the addressed links, then advance both links. */
static void swloop_pump(IsoTpLink **links, const uint32_t *rx_ids, int n)
{
    swframe_t f;
    int i;

    while (swq_pop(&f)) {
        for (i = 0; i < n; i++) {
            if (f.id == rx_ids[i]) {
                isotp_on_can_message(links[i], f.data, f.len);
                break;
            }
        }
    }
    for (i = 0; i < n; i++) {
        isotp_poll(links[i]);
    }
}

int BL_ISOTP_SelfTest(void)
{
    /* Two links: "cmd" sends on 0x7E0, "reply" sends on 0x7E8. */
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
    int got;
    int rc;
    int i;

    /* Arm software loopback and start with an empty ring. */
    g_swq_head = 0;
    g_swq_tail = 0;
    g_swloop_active = 1;

    BL_ISOTP_InitLink(&cmd_link,   BL_ISOTP_ID_CMD,   BL_ISOTP_ID_REPLY,
                      cmd_tx, sizeof(cmd_tx), cmd_rx, sizeof(cmd_rx));
    BL_ISOTP_InitLink(&reply_link, BL_ISOTP_ID_REPLY, BL_ISOTP_ID_CMD,
                      reply_tx, sizeof(reply_tx), reply_rx, sizeof(reply_rx));

    rc = 0;
    if (isotp_send(&cmd_link, msg, sizeof(msg)) != ISOTP_RET_OK) {
        rc = 1;   /* isotp_send rejected the message (library/glue) */
        goto done;
    }

    /* Pump both links until the reply side has the whole message (or we give up). */
    got = 0;
    for (guard = 0; guard < 200000U; guard++) {
        swloop_pump(links, rx_ids, 2);
        if (isotp_receive(&reply_link, out, sizeof(out), &out_len) == ISOTP_RET_OK) {
            got = 1;
            break;
        }
    }

    if (!got) {
        if (reply_link.receive_size == 0U && reply_link.receive_offset == 0U) {
            rc = 2;   /* receiver never saw the First Frame -> routing */
        } else if (cmd_link.send_offset < cmd_link.send_size) {
            rc = 3;   /* sender stalled after FF -> no Flow Control / CF flow */
        } else {
            rc = 4;   /* frames moved but reassembly never completed */
        }
        goto done;
    }

    if (out_len != sizeof(msg)) {
        rc = 5;       /* completed but wrong length */
        goto done;
    }
    for (i = 0; i < (int)sizeof(msg); i++) {
        if (out[i] != msg[i]) {
            rc = 6;   /* payload corrupted in transit */
            goto done;
        }
    }
    rc = 0;           /* PASS */

done:
    g_swloop_active = 0;
    return rc;
}
