/**
 ******************************************************************************
 * @file    can_bl.c
 * @author  Adham Ehab
 * @date    18/08/2026
 * @brief   Minimal CAN driver. Starts in internal LOOPBACK mode so the whole
 *          stack can be built and tested on one board with no transceiver.
 *          Flip CAN_BL_LOOPBACK to 0 for the real two-board bus.
 ******************************************************************************
 */
#include "can_bl.h"
#include "can.h"    /* hcan, configured by CubeMX */

/* 1 = internal loopback (no transceiver). 0 = real CAN bus. */
#define CAN_BL_LOOPBACK   0

void CAN_BL_Init(void)
{
    /* Re-init CAN with our chosen mode (overrides the CubeMX default). */
    HAL_CAN_DeInit(&hcan);
    hcan.Init.Mode = (CAN_BL_LOOPBACK) ? CAN_MODE_LOOPBACK : CAN_MODE_NORMAL;
    HAL_CAN_Init(&hcan);

    /* Accept every message ID into RX FIFO 0 (mask of 0 matches all IDs). */
    CAN_FilterTypeDef filter = {0};
    filter.FilterBank           = 0;
    filter.FilterMode           = CAN_FILTERMODE_IDMASK;
    filter.FilterScale          = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh         = 0x0000;
    filter.FilterIdLow          = 0x0000;
    filter.FilterMaskIdHigh     = 0x0000;
    filter.FilterMaskIdLow      = 0x0000;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation     = ENABLE;
    HAL_CAN_ConfigFilter(&hcan, &filter);

    HAL_CAN_Start(&hcan);
}

int CAN_BL_SelfTest(void)
{
    CAN_TxHeaderTypeDef tx = {0};
    CAN_RxHeaderTypeDef rx = {0};
    uint8_t  tx_data[8] = { 'C', 'A', 'N', '-', 'L', 'O', 'O', 'P' };
    uint8_t  rx_data[8] = {0};
    uint32_t mailbox;
    uint32_t timeout;
    int i;

    tx.StdId = 0x123;          /* the message ID  */
    tx.IDE   = CAN_ID_STD;     /* standard 11-bit ID */
    tx.RTR   = CAN_RTR_DATA;   /* a data frame     */
    tx.DLC   = 8;              /* 8 data bytes     */

    /* hand the frame to a free TX mailbox */
    if (HAL_CAN_AddTxMessage(&hcan, &tx, tx_data, &mailbox) != HAL_OK) {
        return 0;
    }

    /* in loopback the frame comes straight back into RX FIFO 0 */
    timeout = 200000U;
    while (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) == 0U) {
        if (timeout-- == 0U) return 0;   /* nothing came back */
    }

    if (HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &rx, rx_data) != HAL_OK) {
        return 0;
    }

    /* did we get back exactly what we sent? */
    if (rx.StdId != 0x123U || rx.DLC != 8U) return 0;
    for (i = 0; i < 8; i++) {
        if (rx_data[i] != tx_data[i]) return 0;
    }
    return 1;
}

/* ==========================================================================
 *  ISO-TP (ISO 15765-2) over CAN, with flow control.
 *
 *  A CAN data frame carries at most 8 bytes, so a longer message is split into
 *  several frames. The frame TYPE lives in the high nibble of byte 0:
 *
 *      0x0n  Single Frame (SF)  - the whole message; n = its length (1..7)
 *      0x1L LL  First Frame (FF) - starts a long message; 12-bit total length,
 *                                  then the first 6 data bytes
 *      0x2s  Consecutive (CF)   - the remaining chunks; s = a 4-bit sequence
 *                                  number, then up to 7 data bytes
 *      0x3f  Flow Control (FC)  - the RECEIVER pacing the sender; f = status
 *                                  (0 = clear to send, 1 = wait, 2 = overflow)
 *
 *  We run flow control in "lockstep" (BlockSize 1): the receiver asks for one
 *  Consecutive Frame at a time, so only one CF is ever in flight and the small
 *  3-message hardware RX FIFO can never overrun on a large transfer.
 * ========================================================================== */

/* Put one raw CAN frame on the bus. Returns 1 on success, 0 if no mailbox frees up. */
static int cantp_send_frame(uint32_t can_id, const uint8_t *bytes, uint8_t length)
{
    CAN_TxHeaderTypeDef header = {0};
    uint32_t mailbox;
    uint32_t timeout = 100000U;

    header.StdId = can_id;
    header.IDE   = CAN_ID_STD;
    header.RTR   = CAN_RTR_DATA;
    header.DLC   = length;

    while (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0U) {
        if (timeout-- == 0U) {
            return 0;
        }
    }
    return (HAL_CAN_AddTxMessage(&hcan, &header, (uint8_t *)bytes, &mailbox) == HAL_OK);
}

/* Read one raw CAN frame. Spins until a frame arrives or the counter hits 0. */
static int cantp_recv_frame(uint8_t *bytes, uint32_t *timeout)
{
    CAN_RxHeaderTypeDef header;

    while (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) == 0U) {
        if ((*timeout)-- == 0U) {
            return 0;
        }
    }
    return (HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &header, bytes) == HAL_OK);
}

/*
 * Block until the receiver sends its Flow Control frame, then read how it wants
 * the transfer paced (block_size = CFs allowed before the next FC, 0 = all;
 * st_min = minimum gap between CFs). Returns 1 = clear to send, 0 = timeout/abort.
 */
static int cantp_wait_fc(uint32_t timeout, uint8_t *block_size, uint8_t *st_min)
{
    uint8_t frame[8];

    for (;;) {
        uint32_t remaining = timeout;

        if (!cantp_recv_frame(frame, &remaining)) {
            return 0;                               /* nothing arrived in time */
        }
        if ((frame[0] & 0xF0U) == 0x30U) {          /* a Flow Control frame? */
            uint8_t flow_status = frame[0] & 0x0FU;

            if (flow_status == 0x00U) {             /* Clear To Send */
                *block_size = frame[1];
                *st_min     = frame[2];
                return 1;
            }
            if (flow_status == 0x02U) {             /* Overflow: receiver can't take it */
                return 0;
            }
            /* flow_status == 0x01 is "Wait": keep looping for the next FC */
        }
    }
}

/*
 * Send a message of any length to `can_id` using ISO-TP with flow control.
 * Returns 1 once the whole message is on the wire, 0 on any failure.
 */
int CANTP_Send(uint32_t can_id, const uint8_t *data, uint32_t length)
{
    uint8_t  frame[8];
    uint32_t offset;
    uint32_t chunk;
    uint32_t i;
    uint8_t  seq_num       = 1U;   /* Consecutive Frame counter (wraps 0..15) */
    uint8_t  block_size    = 0U;   /* from the receiver's Flow Control */
    uint8_t  st_min        = 0U;
    uint8_t  sent_in_block = 0U;   /* CFs sent since the last Flow Control */

    /* ---- short message: a single frame, no flow control needed ---- */
    if (length <= 7U) {
        frame[0] = (uint8_t)length;                         /* SF: 0x0n, n = length */
        for (i = 0; i < length; i++) {
            frame[1 + i] = data[i];
        }
        return cantp_send_frame(can_id, frame, (uint8_t)(1U + length));
    }

    /* ---- long message: First Frame carries the 12-bit length + first 6 bytes ---- */
    frame[0] = (uint8_t)(0x10U | ((length >> 8) & 0x0FU));
    frame[1] = (uint8_t)(length & 0xFFU);
    for (i = 0; i < 6U; i++) {
        frame[2 + i] = data[i];
    }
    if (!cantp_send_frame(can_id, frame, 8U)) {
        return 0;
    }

    /* the receiver has to say "go" before we stream the rest */
    if (!cantp_wait_fc(2000000U, &block_size, &st_min)) {
        return 0;
    }

    /* ---- Consecutive Frames for everything after the first 6 bytes ---- */
    offset = 6U;
    while (offset < length) {
        frame[0] = (uint8_t)(0x20U | (seq_num & 0x0FU));    /* CF: 0x2s, s = sequence */
        chunk = (length - offset > 7U) ? 7U : (length - offset);
        for (i = 0; i < chunk; i++) {
            frame[1 + i] = data[offset + i];
        }
        if (!cantp_send_frame(can_id, frame, (uint8_t)(1U + chunk))) {
            return 0;
        }
        offset += chunk;
        seq_num++;

        if (st_min != 0U && st_min <= 0x7FU) {              /* leave the requested gap */
            HAL_Delay(st_min);
        }
        if (block_size != 0U) {                             /* block full? wait for a fresh FC */
            sent_in_block++;
            if (sent_in_block >= block_size) {
                sent_in_block = 0U;
                if (offset < length && !cantp_wait_fc(2000000U, &block_size, &st_min)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

/*
 * Receive one ISO-TP message and reassemble it into `data`/`length`.
 * `fc_id` is the CAN ID we send our own Flow Control frames on.
 * Returns 1 on a complete message, 0 on timeout or a protocol error.
 */
int CANTP_Recv(uint8_t *data, uint32_t *length, uint32_t timeout, uint32_t fc_id)
{
    uint8_t  frame[8];
    uint32_t total;
    uint32_t offset;
    uint32_t chunk;
    uint32_t i;

    if (!cantp_recv_frame(frame, &timeout)) {
        return 0;
    }

    /* ---- Single Frame: the whole message is right here ---- */
    if ((frame[0] & 0xF0U) == 0x00U) {
        uint32_t payload_len = frame[0] & 0x0FU;
        for (i = 0; i < payload_len; i++) {
            data[i] = frame[1 + i];
        }
        *length = payload_len;
        return 1;
    }

    /* ---- First Frame: read the total length + first 6 bytes, then pull the rest ---- */
    if ((frame[0] & 0xF0U) == 0x10U) {
        total = ((uint32_t)(frame[0] & 0x0FU) << 8) | frame[1];
        for (i = 0; i < 6U; i++) {
            data[i] = frame[2 + i];
        }
        offset = 6U;

        while (offset < total) {
            /* Lockstep flow control: tell the sender it may send exactly ONE
               Consecutive Frame (BlockSize 1), then read that one frame. Only one
               CF is ever in flight, so the 3-message RX FIFO can't overrun. */
            uint8_t fc[3] = { 0x30U, 0x01U, 0x00U };        /* FC: Clear To Send, BS=1, STmin=0 */
            cantp_send_frame(fc_id, fc, 3U);

            if (!cantp_recv_frame(frame, &timeout)) {
                return 0;
            }
            if ((frame[0] & 0xF0U) != 0x20U) {              /* must be a Consecutive Frame */
                return 0;
            }
            chunk = (total - offset > 7U) ? 7U : (total - offset);
            for (i = 0; i < chunk; i++) {
                data[offset + i] = frame[1 + i];
            }
            offset += chunk;
        }
        *length = total;
        return 1;
    }
    return 0;   /* not an SF or FF - nothing we can use */
}

/*
 * Non-blocking wrapper: return 0 immediately if no frame is waiting, otherwise
 * receive the whole message. `fc_id` = the CAN ID we send Flow Control on.
 */
int CANTP_RecvNB(uint8_t *data, uint32_t *length, uint32_t fc_id)
{
    if (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) == 0U) {
        return 0;
    }
    return CANTP_Recv(data, length, 500000U, fc_id);
}

int CANTP_SelfTest(void)
{
    return 0;   /* defunct on the real bus: flow control needs a second node, not loopback */
}
