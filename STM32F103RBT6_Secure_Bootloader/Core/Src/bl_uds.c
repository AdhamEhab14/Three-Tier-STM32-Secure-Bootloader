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
#include "bootloader.h" /* SLOT_B_BASE, APP_MAX_SIZE - the flash map */
#include "flash_if.h"   /* FlashIf_ErasePages / FlashIf_Write */
#include "can.h"        /* hcan */
#include "isotp.h"      /* isotp_send / isotp_receive / isotp_init_link */
#include "iso14229.h"   /* UDS server + UDSTp transport interface */
#include <string.h>

/* UDS addressing: requester -> FBL on 0x7E0, FBL -> requester on 0x7E8. */
#define BL_UDS_ID_REQUEST   BL_ISOTP_ID_CMD     /* 0x7E0 */
#define BL_UDS_ID_REPLY     BL_ISOTP_ID_REPLY   /* 0x7E8 */

/* Size of the server link's message buffers (matches the UDS server buffers). */
#define BL_UDS_LINK_BUF     256U

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

/* ==========================================================================
 *  Reprogramming services (0x31 erase, 0x34/0x36/0x37 download, 0x11 reset).
 *
 *  A new image is streamed into the A/B staging slot (Slot B); the existing
 *  command layer still owns the Ed25519 verify + copy-into-Slot-A swap. These
 *  services only run once security is unlocked in a programming session.
 * ========================================================================== */
#define BL_UDS_DL_BASE     SLOT_B_BASE     /* staging slot base (0x08015000) */
#define BL_UDS_DL_SIZE     APP_MAX_SIZE    /* bytes the slot can hold         */
#define BL_UDS_FLASH_PAGE  1024U           /* F103 page size                  */
#define BL_UDS_MAX_BLOCK   128U            /* TransferData message cap (fits our buffers) */
#define BL_UDS_RID_ERASE   0xFF00U         /* routineIdentifier: erase staging slot */

static uint32_t g_dl_addr;   /* next flash write address during a download */

/* True only after SecurityAccess unlocked level 1 in a programming session. */
static int bl_uds_reprogramming_allowed(const UDSServer_t *srv)
{
    return (srv->securityLevel == BL_UDS_SEC_LEVEL) &&
           (srv->sessionType == UDS_LEV_DS_PRGS);
}

/* Reset the MCU (target only; a no-op in the host self-test build). */
static void bl_uds_system_reset(void)
{
#if defined(STM32F103xB)
    NVIC_SystemReset();
#endif
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

    case UDS_EVT_RoutineCtrl: {
        UDSRoutineCtrlArgs_t *a = (UDSRoutineCtrlArgs_t *)arg;
        if (!bl_uds_reprogramming_allowed(srv)) {
            return UDS_NRC_SecurityAccessDenied;
        }
        if (a->id == BL_UDS_RID_ERASE && a->ctrlType == UDS_LEV_RCTP_STR) {
            unsigned long pages = BL_UDS_DL_SIZE / BL_UDS_FLASH_PAGE;
            if (FlashIf_ErasePages(BL_UDS_DL_BASE, pages) != 1) {
                return UDS_NRC_GeneralProgrammingFailure;
            }
            return UDS_PositiveResponse;
        }
        return UDS_NRC_RequestOutOfRange;
    }

    case UDS_EVT_RequestDownload: {
        UDSRequestDownloadArgs_t *a = (UDSRequestDownloadArgs_t *)arg;
        uint32_t addr = (uint32_t)(uintptr_t)a->addr;
        if (!bl_uds_reprogramming_allowed(srv)) {
            return UDS_NRC_SecurityAccessDenied;
        }
        /* The image may only land inside the staging slot. */
        if (addr < BL_UDS_DL_BASE ||
            a->size == 0U ||
            a->size > BL_UDS_DL_SIZE ||
            (addr + a->size) > (BL_UDS_DL_BASE + BL_UDS_DL_SIZE)) {
            return UDS_NRC_RequestOutOfRange;
        }
        g_dl_addr = addr;
        a->maxNumberOfBlockLength = BL_UDS_MAX_BLOCK;   /* keep blocks inside our buffers */
        return UDS_PositiveResponse;
    }

    case UDS_EVT_TransferData: {
        UDSTransferDataArgs_t *a = (UDSTransferDataArgs_t *)arg;
        if (FlashIf_Write(g_dl_addr, a->data, a->len) != 1) {
            return UDS_NRC_GeneralProgrammingFailure;
        }
        g_dl_addr += a->len;
        return UDS_PositiveResponse;
    }

    case UDS_EVT_RequestTransferExit:
        return UDS_PositiveResponse;   /* nothing else to finalise here */

    case UDS_EVT_EcuReset: {
        UDSECUResetArgs_t *a = (UDSECUResetArgs_t *)arg;
        a->powerDownTimeMillis = 20U;  /* reset shortly after the reply is sent */
        return UDS_PositiveResponse;
    }

    case UDS_EVT_DoScheduledReset:
        bl_uds_system_reset();
        return UDS_PositiveResponse;

    /* Housekeeping notifications - no request to answer. */
    case UDS_EVT_Err:
    case UDS_EVT_SessionTimeout:
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

    /* 4) RoutineControl start -> erase the staging slot (routineId 0xFF00). */
    req[0] = 0x31U;
    req[1] = UDS_LEV_RCTP_STR;
    req[2] = (uint8_t)(BL_UDS_RID_ERASE >> 8);
    req[3] = (uint8_t)(BL_UDS_RID_ERASE);
    n = bl_uds_tester_xfer(&tester, req, 4U, resp, sizeof(resp));
    if (n < 2U || resp[0] != 0x71U || resp[1] != UDS_LEV_RCTP_STR) {
        rc = 5;   /* erase routine rejected */
        goto done;
    }

    /* 5) RequestDownload: 8 bytes to the staging slot base.
          [0x34][dfi=0][ALFID=0x44][addr:4 BE][size:4 BE] */
    {
        uint8_t dl[11] = {
            0x34U, 0x00U, 0x44U,
            (uint8_t)(BL_UDS_DL_BASE >> 24), (uint8_t)(BL_UDS_DL_BASE >> 16),
            (uint8_t)(BL_UDS_DL_BASE >> 8),  (uint8_t)(BL_UDS_DL_BASE),
            0x00U, 0x00U, 0x00U, 0x08U        /* size = 8 */
        };
        n = bl_uds_tester_xfer(&tester, dl, sizeof(dl), resp, sizeof(resp));
    }
    if (n < 1U || resp[0] != 0x74U) {
        rc = 6;   /* request download rejected */
        goto done;
    }

    /* 6) TransferData block #1: [0x36][BSC=1][8 payload bytes]. */
    {
        uint8_t td[10] = { 0x36U, 0x01U,
                           0xDEU, 0xADU, 0xBEU, 0xEFU, 0x11U, 0x22U, 0x33U, 0x44U };
        n = bl_uds_tester_xfer(&tester, td, sizeof(td), resp, sizeof(resp));
    }
    if (n < 2U || resp[0] != 0x76U || resp[1] != 0x01U) {
        rc = 7;   /* transfer data rejected */
        goto done;
    }

    /* 7) RequestTransferExit: [0x37]. */
    req[0] = 0x37U;
    n = bl_uds_tester_xfer(&tester, req, 1U, resp, sizeof(resp));
    if (n < 1U || resp[0] != 0x77U) {
        rc = 8;   /* transfer exit rejected */
        goto done;
    }

    rc = 0;   /* full unlock + erase + one download block + exit all succeeded */

done:
    BL_ISOTP_SwDisarm();
    return rc;
}
