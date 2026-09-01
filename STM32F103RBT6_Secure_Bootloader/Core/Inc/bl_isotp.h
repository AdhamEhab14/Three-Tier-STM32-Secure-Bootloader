/**
 ******************************************************************************
 * @file    bl_isotp.h
 * @author  Adham Ehab
 * @brief   Glue between the vendored isotp-c library (ISO 15765-2) and this
 *          board's CAN1 peripheral. Provides the three user callbacks isotp-c
 *          requires, a poll/RX pump that routes frames to links by CAN ID, and
 *          a one-board loopback self-test that exercises multi-frame transfer.
 *
 *          This is the standards-compliant transport that backs the iso14229
 *          UDS server; it lives alongside the hand-rolled can_bl transport and
 *          does not replace it. See bl_config.h / BL_USE_ISO_STACK.
 ******************************************************************************
 */
#ifndef BL_ISOTP_H_
#define BL_ISOTP_H_

#include "main.h"
#include "isotp.h"

/* Same UDS-style addressing the hand-rolled transport uses. */
#define BL_ISOTP_ID_CMD    0x7E0U   /* requester -> FBL */
#define BL_ISOTP_ID_REPLY  0x7E8U   /* FBL -> requester */

/*
 * Initialise one ISO-TP link. `tx_id` is the CAN ID this link transmits on;
 * `rx_id` is the CAN ID whose frames the RX pump should hand to this link.
 * The caller owns the send/receive buffers (their size caps the message size).
 */
void BL_ISOTP_InitLink(IsoTpLink *link, uint32_t tx_id, uint32_t rx_id,
                       uint8_t *sendbuf, uint32_t sendbufsize,
                       uint8_t *recvbuf, uint32_t recvbufsize);

/*
 * Drain CAN RX FIFO 0, dispatch each frame to the matching link (by rx_id),
 * then advance every link's timers. Call this frequently from the main loop.
 * `links`/`rx_ids` are parallel arrays of length `n`.
 */
void BL_ISOTP_Pump(IsoTpLink **links, const uint32_t *rx_ids, int n);

/*
 * One-board self-test: puts CAN1 into internal loopback, sends a 20-byte
 * message from a "command" link to a "reply" link through the full ISO-TP
 * segmentation, and checks it arrives intact. 1 = pass, 0 = fail.
 * Re-initialises CAN1; intended to run at start-up before normal operation.
 */
int BL_ISOTP_SelfTest(void);

#endif /* BL_ISOTP_H_ */
