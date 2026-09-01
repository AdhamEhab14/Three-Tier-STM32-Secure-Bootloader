/**
 ******************************************************************************
 * @file    bl_uds.h
 * @author  Adham Ehab
 * @brief   ISO 14229-1 (UDS) server for the bootloader, running on top of the
 *          isotp-c transport (see bl_isotp). Offered alongside the hand-rolled
 *          command layer, not as a replacement; selected at build time by
 *          BL_USE_ISO_STACK.
 ******************************************************************************
 */
#ifndef BL_UDS_H_
#define BL_UDS_H_

#include "main.h"

/* Bring up the UDS server and bind it to CAN1 (requests on 0x7E0, replies on
   0x7E8). Call once, after the CAN peripheral is initialised. */
void BL_UDS_Init(void);

/* Advance the UDS server: pull any received request, dispatch it, send the
   reply. Call frequently from the main loop (ISO 14229 wants < 5 ms spacing). */
void BL_UDS_Poll(void);

/*
 * Software-loopback self-test: drives a UDS reprogramming-unlock sequence
 * (DiagnosticSessionControl -> programming session, then SecurityAccess
 * seed/key) through the real server and isotp-c, with frames carried in RAM
 * instead of on CAN. Returns 0 on pass, or a stage code:
 *   1 = no response at all (transport / server stalled)
 *   2 = DiagnosticSessionControl not accepted
 *   3 = SecurityAccess seed not granted
 *   4 = SecurityAccess key rejected (not unlocked)
 */
int BL_UDS_SelfTest(void);

#endif /* BL_UDS_H_ */
