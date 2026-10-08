/**
 * @file    bl_udspolicy.c
 * @brief   Diagnostic session rules and service attributes (see bl_udspolicy.h).
 */
#include "bl_udspolicy.h"

#define NRC_SERVICE_NOT_SUPPORTED     0x11U
#define NRC_SECURITY_ACCESS_DENIED    0x33U
#define NRC_NOT_IN_ACTIVE_SESSION     0x7FU

#define DEF   BL_SESS_BIT(BL_SESS_DEFAULT)
#define PRG   BL_SESS_BIT(BL_SESS_PROGRAMMING)
#define EXT   BL_SESS_BIT(BL_SESS_EXTENDED)
#define SAF   BL_SESS_BIT(BL_SESS_SAFETY)

/* sid   sessions            security  addressing */
static const bl_uds_service_t services[] = {
    { 0x10U, BL_SESS_ALL,      0U, BL_ADDR_BOTH     },   /* DiagnosticSessionControl */
    { 0x11U, BL_SESS_ALL,      0U, BL_ADDR_BOTH     },   /* ECUReset */
    { 0x22U, BL_SESS_ALL,      0U, BL_ADDR_BOTH     },   /* ReadDataByIdentifier */
    { 0x23U, PRG,              1U, BL_ADDR_PHYSICAL },   /* ReadMemoryByAddress (read-back) */
    { 0x27U, PRG | EXT | SAF,  0U, BL_ADDR_PHYSICAL },   /* SecurityAccess */
    { 0x28U, EXT | PRG,        0U, BL_ADDR_BOTH     },   /* CommunicationControl */
    { 0x31U, PRG | EXT,        0U, BL_ADDR_PHYSICAL },   /* RoutineControl (each routine adds its own checks) */
    { 0x34U, PRG,              1U, BL_ADDR_PHYSICAL },   /* RequestDownload */
    { 0x36U, PRG,              1U, BL_ADDR_PHYSICAL },   /* TransferData */
    { 0x37U, PRG,              1U, BL_ADDR_PHYSICAL },   /* RequestTransferExit */
    { 0x3EU, BL_SESS_ALL,      0U, BL_ADDR_BOTH     },   /* TesterPresent */
    { 0x85U, EXT | PRG,        0U, BL_ADDR_BOTH     },   /* ControlDTCSetting */
};

int BL_UdsSessionKnown(uint8_t session)
{
    return (session >= BL_SESS_DEFAULT && session <= BL_SESS_SAFETY) ? 1 : 0;
}

int BL_UdsSessionChangeAllowed(uint8_t from, uint8_t to)
{
    if (!BL_UdsSessionKnown(from) || !BL_UdsSessionKnown(to)) return 0;
    if (to == BL_SESS_DEFAULT) return 1;      /* always allowed: it is where everything ends up */
    if (to == from)            return 1;      /* asking for the session we are in */
    if (from == BL_SESS_EXTENDED) return 1;   /* extended is the gate to programming and safety */
    if (from == BL_SESS_DEFAULT && to == BL_SESS_EXTENDED) return 1;
    return 0;
}

const bl_uds_service_t *BL_UdsService(uint8_t sid)
{
    unsigned i;
    for (i = 0U; i < sizeof(services) / sizeof(services[0]); i++)
    {
        if (services[i].sid == sid) return &services[i];
    }
    return (const bl_uds_service_t *)0;
}

uint8_t BL_UdsGate(uint8_t sid, uint8_t session, uint8_t security_level, uint8_t addressing)
{
    const bl_uds_service_t *s = BL_UdsService(sid);

    if (s == (const bl_uds_service_t *)0)   return NRC_SERVICE_NOT_SUPPORTED;
    if ((s->address_mask & addressing) == 0U) return NRC_SERVICE_NOT_SUPPORTED;
    if (!BL_UdsSessionKnown(session) || (s->session_mask & BL_SESS_BIT(session)) == 0U)
        return NRC_NOT_IN_ACTIVE_SESSION;
    if (security_level < s->min_security)   return NRC_SECURITY_ACCESS_DENIED;
    return 0U;
}
