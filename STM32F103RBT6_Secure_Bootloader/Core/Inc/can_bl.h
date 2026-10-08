/**
 ******************************************************************************
 * @file    can_bl.h
 * @author  Adham Ehab
 * @date    18/08/2026
 * @brief   Minimal CAN driver + ISO-TP-style segmentation for the bootloader.
 ******************************************************************************
 */
#ifndef CAN_BL_H_
#define CAN_BL_H_

#include "main.h"

/* UDS-style CAN IDs for the bootloader protocol. */
#define CANBL_ID_CMD    0x7E0U   /* host/requester -> FBL */
#define CANBL_ID_REPLY  0x7E8U   /* FBL -> host/requester */

/* Bring up CAN1 (loopback mode for now), accept-all filter, and start it. */
void CAN_BL_Init(void);

/* Next received frame from the RX interrupt's ring. 1 = got one, 0 = none waiting. */
int      CAN_BL_Read(uint32_t *id, uint8_t *ext, uint8_t data[8], uint8_t *dlc);
uint32_t CAN_BL_RxPending(void);   /* frames waiting in the ring */
uint32_t CAN_BL_RxDropped(void);   /* frames lost because the ring was full */

/* Send one raw frame to ourselves and read it back. 1 = pass, 0 = fail. */
int  CAN_BL_SelfTest(void);

/* ISO-TP-style transport: send / receive a multi-byte message over CAN. */
int  CANTP_Send(uint32_t id, const uint8_t *data, uint32_t len);          /* waits for FC on multi-frame */
int  CANTP_Recv(uint8_t *data, uint32_t *len, uint32_t timeout, uint32_t fc_id);
int  CANTP_RecvNB(uint8_t *data, uint32_t *len, uint32_t fc_id);          /* non-blocking: 0 if nothing waiting */

/* Loopback test of the segmentation: send a 20-byte message to ourselves. */
int  CANTP_SelfTest(void);

#endif /* CAN_BL_H_ */
