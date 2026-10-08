/**
 * @file    bl_secaccess.h
 * @brief   Brute-force policy for SecurityAccess (UDS 0x27), shared by both UDS servers.
 *
 * The caller passes the time in, so nothing here touches the HAL and the policy can be
 * unit tested on a PC.
 *
 *   power-up   no 0x27 for BL_SEC_BOOT_DELAY_MS (NRC 0x37)
 *   wrong key  wait BL_SEC_FAIL_DELAY_MS before the next try. From the 3rd wrong key in a
 *              row the wait is BL_SEC_LOCKOUT_MS (NRC 0x36 meanwhile). Only a correct
 *              key clears the count.
 */
#ifndef BL_SECACCESS_H
#define BL_SECACCESS_H

#include <stdint.h>

#define BL_SEC_BOOT_DELAY_MS   1000U
#define BL_SEC_FAIL_DELAY_MS   1000U
#define BL_SEC_MAX_FAILS       3U
#define BL_SEC_LOCKOUT_MS      10000U

#define BL_SEC_NRC_TIME_DELAY  0x37U   /* requiredTimeDelayNotExpired */
#define BL_SEC_NRC_EXCEEDED    0x36U   /* exceededNumberOfAttempts */

typedef struct {
    uint32_t boot_until;    /* no SecurityAccess before this time */
    uint32_t retry_after;   /* no attempt before this time (set by a wrong key) */
    uint32_t fails;         /* wrong keys in a row */
    uint32_t seed_counter;  /* goes into every seed so none repeats within a power-up */
} bl_sec_t;

void     BL_SecInit(bl_sec_t *s, uint32_t now_ms);

/* 0 = carry on, otherwise the NRC to answer with. Ask before handling any 0x27 request. */
uint8_t  BL_SecGate(const bl_sec_t *s, uint32_t now_ms);

void     BL_SecNoteFail(bl_sec_t *s, uint32_t now_ms);
void     BL_SecNoteSuccess(bl_sec_t *s);

/* The wait the policy applies after the failure just recorded. For libraries that apply
   the delay themselves (iso14229). */
uint32_t BL_SecFailDelayMs(const bl_sec_t *s);

/* 1, 2, 3, ... */
uint32_t BL_SecNextCounter(bl_sec_t *s);

#endif /* BL_SECACCESS_H */
