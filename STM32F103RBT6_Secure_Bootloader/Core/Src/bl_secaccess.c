/**
 * @file    bl_secaccess.c
 * @brief   SecurityAccess brute-force policy (see bl_secaccess.h).
 */
#include "bl_secaccess.h"

/* True while t is still ahead of now. Written as a signed difference so it survives the
   32-bit tick wrapping. */
static int pending(uint32_t now, uint32_t t)
{
    return ((int32_t)(t - now) > 0) ? 1 : 0;
}

void BL_SecInit(bl_sec_t *s, uint32_t now_ms)
{
    s->boot_until   = now_ms + BL_SEC_BOOT_DELAY_MS;
    s->retry_after  = now_ms;
    s->fails        = 0U;
    s->seed_counter = 0U;
}

uint8_t BL_SecGate(const bl_sec_t *s, uint32_t now_ms)
{
    if (pending(now_ms, s->boot_until))  return (uint8_t)BL_SEC_NRC_TIME_DELAY;
    if (pending(now_ms, s->retry_after)) return (uint8_t)BL_SEC_NRC_EXCEEDED;
    return 0U;
}

uint32_t BL_SecFailDelayMs(const bl_sec_t *s)
{
    return (s->fails >= BL_SEC_MAX_FAILS) ? BL_SEC_LOCKOUT_MS : BL_SEC_FAIL_DELAY_MS;
}

void BL_SecNoteFail(bl_sec_t *s, uint32_t now_ms)
{
    if (s->fails < 0xFFFFFFFFU) s->fails++;
    s->retry_after = now_ms + BL_SecFailDelayMs(s);
}

void BL_SecNoteSuccess(bl_sec_t *s)
{
    s->fails = 0U;
}

uint32_t BL_SecNextCounter(bl_sec_t *s)
{
    return ++s->seed_counter;
}
