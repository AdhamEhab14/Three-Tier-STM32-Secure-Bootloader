/**
 * @file    bl_udspolicy.h
 * @brief   Diagnostic session rules and per-service attributes, shared by both UDS servers.
 *
 * Plain C with no HAL, so it is unit tested on a PC.
 *
 * Sessions (service 0x10) and the changes allowed between them:
 *
 *     default (01)     -> default, extended
 *     extended (03)    -> default, extended, programming, safety
 *     programming (02) -> default, programming
 *     safety (04)      -> default, safety
 *
 * So programming (and safety) can only be reached through the extended session, and the
 * only way out of them is back to default. A reset and the S3 timeout also end up in
 * default; the servers handle that themselves.
 *
 * Every service has an attribute row: the sessions it works in, the security level it
 * needs and whether it accepts physical and/or functional addressing.
 */
#ifndef BL_UDSPOLICY_H
#define BL_UDSPOLICY_H

#include <stdint.h>

#define BL_SESS_DEFAULT      0x01U
#define BL_SESS_PROGRAMMING  0x02U
#define BL_SESS_EXTENDED     0x03U
#define BL_SESS_SAFETY       0x04U

/* One bit per session, for the masks in the service table. */
#define BL_SESS_BIT(s)       (1U << ((s) - 1U))
#define BL_SESS_ALL          (BL_SESS_BIT(BL_SESS_DEFAULT) | BL_SESS_BIT(BL_SESS_PROGRAMMING) | \
                              BL_SESS_BIT(BL_SESS_EXTENDED) | BL_SESS_BIT(BL_SESS_SAFETY))

#define BL_ADDR_PHYSICAL     0x01U
#define BL_ADDR_FUNCTIONAL   0x02U
#define BL_ADDR_BOTH         (BL_ADDR_PHYSICAL | BL_ADDR_FUNCTIONAL)

#define BL_SESSION_TIMEOUT_MS  5000U   /* S3: no request for this long drops back to default */

typedef struct {
    uint8_t sid;
    uint8_t session_mask;    /* BL_SESS_BIT() of every session the service works in */
    uint8_t min_security;    /* SecurityAccess level needed, 0 = none (level n = subfunction 2n-1/2n) */
    uint8_t address_mask;    /* BL_ADDR_* the service accepts */
} bl_uds_service_t;

/* 1 for the four sessions above. */
int BL_UdsSessionKnown(uint8_t session);

/* 1 if a DiagnosticSessionControl request may take the server from one session to another. */
int BL_UdsSessionChangeAllowed(uint8_t from, uint8_t to);

/* The attribute row for a service, or 0 when the service is not implemented. */
const bl_uds_service_t *BL_UdsService(uint8_t sid);

/* The NRC to answer a request with, or 0 when it may go ahead. Checked in this order:
   service implemented and addressing accepted (0x11), allowed in this session (0x7F),
   enough security (0x33). */
uint8_t BL_UdsGate(uint8_t sid, uint8_t session, uint8_t security_level, uint8_t addressing);

#endif /* BL_UDSPOLICY_H */
