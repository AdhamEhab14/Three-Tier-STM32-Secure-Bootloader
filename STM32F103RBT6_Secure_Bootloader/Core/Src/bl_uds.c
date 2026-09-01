/**
 ******************************************************************************
 * @file    bl_uds.c
 * @author  Adham Ehab
 * @brief   iso14229 (ISO 14229-1 UDS) server bound to the isotp-c transport.
 *          See bl_uds.h. This build implements the reprogramming subset one
 *          service at a time; DiagnosticSessionControl (0x10) is in first.
 ******************************************************************************
 */
#include "bl_uds.h"
#include "bl_isotp.h"   /* IsoTpLink, BL_ISOTP_* helpers + CAN IDs */
#include "can.h"        /* hcan */
#include "isotp.h"      /* isotp_send / isotp_receive / isotp_init_link */
#include "iso14229.h"   /* UDS server + UDSTp transport interface */
#include <string.h>

/* UDS addressing: requester -> FBL on 0x7E0, FBL -> requester on 0x7E8. */
#define BL_UDS_ID_REQUEST   BL_ISOTP_ID_CMD     /* 0x7E0 */
#define BL_UDS_ID_REPLY     BL_ISOTP_ID_REPLY   /* 0x7E8 */

/* Size of the server link's message buffers (matches the UDS server buffers). */
#define BL_UDS_LINK_BUF     512U

/* ==========================================================================
 *  Time base required by iso14229 under UDS_SYS_CUSTOM.
 * ========================================================================== */
uint32_t UDSMillis(void)
{
    return HAL_GetTick();
}

/* ==========================================================================
 *  Transport bridge: an iso14229 UDSTp_t backed by one isotp-c link.
 *  iso14229 requires the UDSTp_t to sit at offset 0 of the handle struct so it
 *  can cast between the two.
 * ========================================================================== */
typedef struct {
    UDSTp_t    hdl;    /* MUST be the first member */
    IsoTpLink *link;   /* the server's ISO-TP link */
    uint32_t   rx_id;  /* CAN ID whose frames belong to this link */
} bl_uds_tp_t;

/* Send a whole UDS message: hand it to ISO-TP as one payload. */
static UDSTpSize_t bl_uds_tp_send(UDSTp_t *hdl, const uint8_t *buf, size_t len,
                                  const UDSSDU_t *info)
{
    bl_uds_tp_t *tp = (bl_uds_tp_t *)hdl;
    (void)info;   /* single physical channel: addressing is fixed */

    if (isotp_send(tp->link, buf, (uint32_t)len) != ISOTP_RET_OK) {
        return -1;
    }
    return (UDSTpSize_t)len;
}

/* Receive one reassembled UDS message, or 0 if none is complete yet. */
static UDSTpSize_t bl_uds_tp_recv(UDSTp_t *hdl, uint8_t *buf, size_t bufsize,
                                  UDSSDU_t *info)
{
    bl_uds_tp_t *tp = (bl_uds_tp_t *)hdl;
    uint32_t out_len = 0;

    if (isotp_receive(tp->link, buf, (uint32_t)bufsize, &out_len) != ISOTP_RET_OK) {
        return 0;
    }
    if (info != NULL) {
        info->A_TA_Type = UDS_A_TA_TYPE_PHYSICAL;
        info->A_SA      = BL_UDS_ID_REQUEST;
        info->A_TA      = BL_UDS_ID_REPLY;
    }
    return (UDSTpSize_t)out_len;
}

/* Feed the link from CAN, then advance it. During the software self-test the
   ring is pumped elsewhere and the CAN FIFO is empty, so this is a harmless
   no-op there. */
static UDSTpStatus_t bl_uds_tp_poll(UDSTp_t *hdl)
{
    bl_uds_tp_t *tp = (bl_uds_tp_t *)hdl;

    BL_ISOTP_Pump(&tp->link, &tp->rx_id, 1);
    return UDS_TP_IDLE;
}

/* ==========================================================================
 *  UDS server instance
 * ========================================================================== */
static UDSServer_t g_srv;
static bl_uds_tp_t g_tp;
static IsoTpLink   g_link;
static uint8_t     g_link_tx[BL_UDS_LINK_BUF];
static uint8_t     g_link_rx[BL_UDS_LINK_BUF];

/* ==========================================================================
 *  Security access (0x27) - seed/key gate for the reprogramming services.
 *
 *  requestSeed = subfunction 0x01, sendKey = subfunction 0x02, unlocking
 *  security level 0x01. The seed/key relation here is a lightweight obfuscation,
 *  NOT the project's cryptographic root of trust: firmware images are still
 *  authenticated by their Ed25519 signature at install time. This gate only
 *  decides whether a UDS client may drive the download services.
 * ========================================================================== */
#define BL_UDS_SEC_LEVEL   0x01U

static uint8_t g_seed[4];   /* last seed handed out, awaiting its key */

/* Build a non-zero 4-byte seed from the millisecond tick. */
static void bl_uds_make_seed(uint8_t seed[4])
{
    uint32_t s = (UDSMillis() * 2654435761U) ^ 0x9E3779B9U;

    if (s == 0U) {
        s = 0xA5A5A5A5U;   /* spec: never hand out an all-zero seed for a locked level */
    }
    seed[0] = (uint8_t)(s);
    seed[1] = (uint8_t)(s >> 8);
    seed[2] = (uint8_t)(s >> 16);
    seed[3] = (uint8_t)(s >> 24);
}

/* Derive the expected key from a seed (key[i] = seed[i] XOR shared secret[i]). */
static void bl_uds_key_from_seed(const uint8_t seed[4], uint8_t key[4])
{
    static const uint8_t secret[4] = { 0x19U, 0x84U, 0xC0U, 0xDEU };
    int i;

    for (i = 0; i < 4; i++) {
        key[i] = (uint8_t)(seed[i] ^ secret[i]);
    }
}

/* Server event handler. Return UDS_PositiveResponse to accept a request (the
   library builds the positive reply), or a negative response code to reject. */
static UDSErr_t bl_uds_fn(UDSServer_t *srv, UDSEvent_t event, void *arg)
{
    switch (event) {
    case UDS_EVT_DiagSessCtrl: {
        UDSDiagSessCtrlArgs_t *a = (UDSDiagSessCtrlArgs_t *)arg;
        switch (a->type) {
        case UDS_LEV_DS_DS:      /* default session */
        case UDS_LEV_DS_PRGS:    /* programming session */
        case UDS_LEV_DS_EXTDS:   /* extended diagnostic session */
            return UDS_PositiveResponse;
        default:
            return UDS_NRC_SubFunctionNotSupported;
        }
    }

    case UDS_EVT_SecAccessRequestSeed: {
        UDSSecAccessRequestSeedArgs_t *a = (UDSSecAccessRequestSeedArgs_t *)arg;
        if (a->level != BL_UDS_SEC_LEVEL) {
            return UDS_NRC_SubFunctionNotSupported;
        }
        bl_uds_make_seed(g_seed);
        (void)a->copySeed(srv, g_seed, sizeof(g_seed));   /* append seed to the reply */
        return UDS_PositiveResponse;
    }

    case UDS_EVT_SecAccessValidateKey: {
        UDSSecAccessValidateKeyArgs_t *a = (UDSSecAccessValidateKeyArgs_t *)arg;
        uint8_t expect[4];
        if (a->level != BL_UDS_SEC_LEVEL) {
            return UDS_NRC_SubFunctionNotSupported;
        }
        if (a->len != sizeof(expect)) {
            return UDS_NRC_InvalidKey;
        }
        bl_uds_key_from_seed(g_seed, expect);
        if (memcmp(a->key, expect, sizeof(expect)) != 0) {
            return UDS_NRC_InvalidKey;
        }
        return UDS_PositiveResponse;   /* library records the unlocked level */
    }

    /* Housekeeping notifications - no request to answer. */
    case UDS_EVT_Err:
    case UDS_EVT_SessionTimeout:
    case UDS_EVT_DoScheduledReset:
        return UDS_PositiveResponse;

    /* Services not implemented in this build yet. */
    default:
        return UDS_NRC_ServiceNotSupported;
    }
}

void BL_UDS_Init(void)
{
    /* Server link: transmits replies on 0x7E8, receives requests on 0x7E0. */
    isotp_init_link(&g_link, BL_UDS_ID_REPLY,
                    g_link_tx, sizeof(g_link_tx),
                    g_link_rx, sizeof(g_link_rx));

    g_tp.hdl.send = bl_uds_tp_send;
    g_tp.hdl.recv = bl_uds_tp_recv;
    g_tp.hdl.poll = bl_uds_tp_poll;
    g_tp.link     = &g_link;
    g_tp.rx_id    = BL_UDS_ID_REQUEST;

    UDSServerInit(&g_srv);
    g_srv.tp = &g_tp.hdl;
    g_srv.fn = bl_uds_fn;
}

void BL_UDS_Poll(void)
{
    UDSServerPoll(&g_srv);
}

/* ==========================================================================
 *  Software-loopback self-test
 * ========================================================================== */

/* Send one UDS request from `tester` and wait for the reassembled response,
   pumping the software ring and the server meanwhile. Returns the response
   length, or 0 if none arrived within the budget. */
static uint32_t bl_uds_tester_xfer(IsoTpLink *tester,
                                   const uint8_t *req, uint32_t req_len,
                                   uint8_t *resp, uint32_t resp_cap)
{
    uint32_t out_len = 0;
    uint32_t guard;

    if (isotp_send(tester, req, req_len) != ISOTP_RET_OK) {
        return 0;
    }
    for (guard = 0; guard < 200000U; guard++) {
        BL_ISOTP_SwPump();       /* carry frames between the two links */
        UDSServerPoll(&g_srv);   /* let the server consume the request and answer */
        if (isotp_receive(tester, resp, resp_cap, &out_len) == ISOTP_RET_OK) {
            return out_len;
        }
    }
    return 0;
}

int BL_UDS_SelfTest(void)
{
    /* A tester link that plays the diagnostic client for the exchange. */
    IsoTpLink tester;
    uint8_t   tester_tx[64];
    uint8_t   tester_rx[64];

    IsoTpLink *links[2]  = { &g_link,           &tester };
    uint32_t   rx_ids[2] = { BL_UDS_ID_REQUEST, BL_UDS_ID_REPLY };
    /* server link receives requests on 0x7E0; tester receives replies on 0x7E8. */

    uint8_t  req[8];
    uint8_t  resp[64];
    uint8_t  seed[4];
    uint8_t  key[4];
    uint32_t n;
    int      rc = 0;

    BL_UDS_Init();
    BL_ISOTP_InitLink(&tester, BL_UDS_ID_REQUEST, BL_UDS_ID_REPLY,
                      tester_tx, sizeof(tester_tx), tester_rx, sizeof(tester_rx));

    BL_ISOTP_SwArm(links, rx_ids, 2);

    /* 1) DiagnosticSessionControl -> programming session. */
    req[0] = 0x10U;
    req[1] = UDS_LEV_DS_PRGS;
    n = bl_uds_tester_xfer(&tester, req, 2U, resp, sizeof(resp));
    if (n == 0U) {
        rc = 1;   /* no response at all -> transport/server stalled */
        goto done;
    }
    if (n < 2U || resp[0] != 0x50U || resp[1] != UDS_LEV_DS_PRGS) {
        rc = 2;   /* session control not accepted */
        goto done;
    }

    /* Security access is blocked for a short boot delay; wait it out so the seed
       request is not rejected with RequiredTimeDelayNotExpired (0x37). */
    while ((int32_t)(UDSMillis() - g_srv.sec_access_boot_delay_timer) <= 0) {
        /* spin: ~1 s of real time on target; advances the tick on host */
    }

    /* 2) SecurityAccess requestSeed. */
    req[0] = 0x27U;
    req[1] = 0x01U;
    n = bl_uds_tester_xfer(&tester, req, 2U, resp, sizeof(resp));
    if (n < 6U || resp[0] != 0x67U || resp[1] != 0x01U) {
        rc = 3;   /* seed not granted */
        goto done;
    }
    seed[0] = resp[2];
    seed[1] = resp[3];
    seed[2] = resp[4];
    seed[3] = resp[5];

    /* 3) SecurityAccess sendKey with the matching key. */
    bl_uds_key_from_seed(seed, key);
    req[0] = 0x27U;
    req[1] = 0x02U;
    req[2] = key[0];
    req[3] = key[1];
    req[4] = key[2];
    req[5] = key[3];
    n = bl_uds_tester_xfer(&tester, req, 6U, resp, sizeof(resp));
    if (n < 2U || resp[0] != 0x67U || resp[1] != 0x02U) {
        rc = 4;   /* key rejected -> not unlocked */
        goto done;
    }

    rc = 0;   /* programming session entered and security unlocked */

done:
    BL_ISOTP_SwDisarm();
    return rc;
}
