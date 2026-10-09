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
#include "bl_seccrypto.h"
#include "bl_secaccess.h"
#include "bl_udspolicy.h"
#include "bl_uds_vectors.h"
#include <string.h>

/* UDS addressing: requester -> FBL on 0x7E0, FBL -> requester on 0x7E8. */
#define BL_UDS_ID_REQUEST   BL_ISOTP_ID_CMD     /* 0x7E0 */
#define BL_UDS_ID_REPLY     BL_ISOTP_ID_REPLY   /* 0x7E8 */
#define BL_UDS_ID_FUNCTIONAL 0x7DFU             /* functional (broadcast) requests, single frame only */

/* Size of the server link's message buffers (matches the UDS server buffers). */
#if defined(BL_UDS_SELFTEST_ON_BOOT) && BL_UDS_SELFTEST_ON_BOOT
#define BL_UDS_LINK_BUF     160U   /* self-test only sends small messages, and RAM is tight next to the SBL */
#else
#define BL_UDS_LINK_BUF     256U
#endif

/* ==========================================================================
 *  Time base required by iso14229 under UDS_SYS_CUSTOM.
 * ========================================================================== */
uint32_t UDSMillis(void)
{
    return HAL_GetTick();
}

static UDSServer_t g_srv;

/* ==========================================================================
 *  Transport bridge: an iso14229 UDSTp_t backed by one isotp-c link.
 *  iso14229 requires the UDSTp_t to sit at offset 0 of the handle struct so it
 *  can cast between the two.
 * ========================================================================== */
typedef struct {
    UDSTp_t    hdl;        /* MUST be the first member */
    IsoTpLink *link[2];    /* [0] physical requests (0x7E0), [1] functional ones (0x7DF) */
    uint32_t   rx_id[2];   /* CAN ID whose frames belong to each link */
} bl_uds_tp_t;

/* Send a whole UDS message: hand it to ISO-TP as one payload. */
static UDSTpSize_t bl_uds_tp_send(UDSTp_t *hdl, const uint8_t *buf, size_t len,
                                  const UDSSDU_t *info)
{
    bl_uds_tp_t *tp = (bl_uds_tp_t *)hdl;
    (void)info;   /* replies always go out on 0x7E8, whichever way the request came in */

    if (isotp_send(tp->link[0], buf, (uint32_t)len) != ISOTP_RET_OK) {
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
    int i;

    for (i = 0; i < 2; i++) {
        if (isotp_receive(tp->link[i], buf, (uint32_t)bufsize, &out_len) != ISOTP_RET_OK) {
            continue;
        }
        /* Every request passes the same gate before the library sees it: the service has to
           accept this addressing, work in the active session and have enough security (see
           bl_udspolicy.c). Doing it here keeps the answer independent of which check the library
           happens to make first. A refused functional request is not answered at all. */
        if (out_len >= 1U) {
            uint8_t nrc = BL_UdsGate(buf[0], g_srv.sessionType, g_srv.securityLevel,
                                     (i == 0) ? BL_ADDR_PHYSICAL : BL_ADDR_FUNCTIONAL);
            if (nrc != 0U) {
                uint8_t neg[3];
                neg[0] = 0x7FU; neg[1] = buf[0]; neg[2] = nrc;
                if (i == 0) {
                    (void)isotp_send(tp->link[0], neg, sizeof(neg));
                }
                g_srv.s3_session_timeout_timer = UDSMillis() + g_srv.s3_ms;   /* it was still a request */
                return 0;
            }
        }
        if (info != NULL) {
            info->A_TA_Type = (i == 0) ? UDS_A_TA_TYPE_PHYSICAL : UDS_A_TA_TYPE_FUNCTIONAL;
            info->A_SA      = BL_UDS_ID_REQUEST;
            info->A_TA      = BL_UDS_ID_REPLY;
        }
        return (UDSTpSize_t)out_len;
    }
    return 0;
}

/* Feed the link from CAN, then advance it. During the software self-test the
   ring is pumped elsewhere and the CAN FIFO is empty, so this is a harmless
   no-op there. */
static UDSTpStatus_t bl_uds_tp_poll(UDSTp_t *hdl)
{
    bl_uds_tp_t *tp = (bl_uds_tp_t *)hdl;

    BL_ISOTP_Pump(tp->link, tp->rx_id, 2);
    return UDS_TP_IDLE;
}

/* ==========================================================================
 *  UDS server instance
 * ========================================================================== */
static bl_uds_tp_t g_tp;
static IsoTpLink   g_link;
static IsoTpLink   g_link_func;                 /* functional requests: one frame, so a tiny buffer */
static uint8_t     g_link_func_tx[8];
static uint8_t     g_link_func_rx[16];
static uint8_t     g_link_tx[BL_UDS_LINK_BUF];
static uint8_t     g_link_rx[BL_UDS_LINK_BUF];

/* ==========================================================================
 *  Security access (0x27) - seed/key gate for the reprogramming services.
 *
 *  requestSeed = subfunction 0x01, sendKey = subfunction 0x02, unlocking
 *  security level 0x01. The key is the first 4 bytes of AES-CMAC(K, seed), K being the
 *  product's SecurityAccess key. That is access control, not the project's root of trust:
 *  firmware images are still authenticated by their Ed25519 signature at install time. This
 *  gate only decides whether a UDS client may drive the download services.
 * ========================================================================== */
#define BL_UDS_SEC_LEVEL   0x01U

static uint8_t g_seed[4];   /* last seed handed out, awaiting its key */
static uint8_t g_seed_valid;
static bl_sec_t g_sec;      /* attempt counter and seed counter (the library applies the delays) */

/* The library waits this long after every wrong key; the policy stretches it once
   BL_SEC_MAX_FAILS have piled up (local patch in iso14229.h). */
uint32_t BL_UdsFailDelayMs(void)
{
    return BL_SecFailDelayMs(&g_sec);
}

/* A fresh seed per request, derived under the SecurityAccess key so it cannot be
   predicted without it. */
static void bl_uds_make_seed(uint8_t seed[4])
{
    uint32_t cycles = 0U;
#if defined(DWT_BASE)
    cycles = DWT->CYCCNT;
#endif
    BL_Sec_MakeSeed(seed, BL_SecNextCounter(&g_sec), UDSMillis(), cycles);
}

/* What the client has to answer: first 4 bytes of AES-CMAC(K, seed). */
static void bl_uds_key_from_seed(const uint8_t seed[4], uint8_t key[4])
{
    BL_Sec_KeyForSeed(seed, key);
}

/* ==========================================================================
 *  Reprogramming services (0x31 erase, 0x34/0x36/0x37 download, 0x11 reset).
 *
 *  A new image is streamed into the A/B staging slot (Slot B); the existing
 *  command layer still owns the Ed25519 verify + copy-into-Slot-A swap. These
 *  services only run once security is unlocked in a programming session.
 * ========================================================================== */
#define BL_UDS_DL_BASE     SLOT_B_BASE     /* staging slot base (0x08015000) */
#define BL_UDS_DL_SIZE     FBL_REGION_SIZE /* bytes the slot can hold (an FBL image fills it) */
#define BL_UDS_FLASH_PAGE  1024U           /* F103 page size                  */
#define BL_UDS_MAX_BLOCK   128U            /* TransferData message cap (fits our buffers) */
#define BL_UDS_RID_ERASE   0xFF00U         /* routineIdentifier: erase staging slot */
#define BL_UDS_RID_CHECK   0xFF01U         /* routineIdentifier: CRC-32 a region (CheckMemory) */

static uint32_t g_dl_addr;   /* next flash write address during a download */

/* Running CRC-32 (reflected, poly 0xEDB88320) over a buffer. */
static uint32_t bl_uds_crc32_step(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    int k;
    for (i = 0U; i < len; i++) {
        crc ^= data[i];
        for (k = 0; k < 8; k++) {
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return crc;
}

/* CRC-32 over a flash region, read back through the flash driver in chunks. */
static uint32_t bl_uds_crc32_region(uint32_t addr, uint32_t size)
{
    uint32_t crc = 0xFFFFFFFFU;
    uint8_t  buf[64];
    uint32_t done = 0U;
    while (done < size) {
        uint32_t n = (size - done > sizeof(buf)) ? (uint32_t)sizeof(buf) : (size - done);
        FlashIf_Read(addr + done, buf, (unsigned long)n);
        crc = bl_uds_crc32_step(crc, buf, n);
        done += n;
    }
    return crc ^ 0xFFFFFFFFU;
}

/* Drop everything a session or an unlock set up: used on a session change and on S3. */
static void bl_uds_forget_session_state(void)
{
    g_seed_valid = 0U;
    g_dl_addr = 0U;
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
        if (!BL_UdsSessionKnown(a->type)) {
            return UDS_NRC_SubFunctionNotSupported;
        }
        if (!BL_UdsSessionChangeAllowed(srv->sessionType, a->type)) {
            return UDS_NRC_ConditionsNotCorrect;
        }
        /* The library keeps the unlock across a session change; a new session starts locked. */
        srv->securityLevel = 0U;
        bl_uds_forget_session_state();
        return UDS_PositiveResponse;
    }

    case UDS_EVT_ReadDataByIdent: {
        /* the identifiers the production command layer also answers (F190 and FD00 need
           its metadata and self-test state, so they stay there) */
        UDSRDBIArgs_t *a = (UDSRDBIArgs_t *)arg;
        switch (a->dataId) {
        case 0xF186U: {   /* active diagnostic session */
            uint8_t session = srv->sessionType;
            return (UDSErr_t)a->copy(srv, &session, 1U);
        }
        case 0xF195U: {   /* bootloader version */
            const uint8_t ver[4] = { BL_VENDOR_ID, BL_SW_MAJOR, BL_SW_MINOR, BL_SW_PATCH };
            return (UDSErr_t)a->copy(srv, ver, sizeof(ver));
        }
        default:
            return UDS_NRC_RequestOutOfRange;
        }
    }

    case UDS_EVT_SecAccessRequestSeed: {
        UDSSecAccessRequestSeedArgs_t *a = (UDSSecAccessRequestSeedArgs_t *)arg;
        if (a->level != BL_UDS_SEC_LEVEL) {
            return UDS_NRC_SubFunctionNotSupported;
        }
        bl_uds_make_seed(g_seed);
        g_seed_valid = 1U;
        (void)a->copySeed(srv, g_seed, sizeof(g_seed));   /* append seed to the reply */
        return UDS_PositiveResponse;
    }

    case UDS_EVT_SecAccessValidateKey: {
        UDSSecAccessValidateKeyArgs_t *a = (UDSSecAccessValidateKeyArgs_t *)arg;
        uint8_t expect[4];
        if (a->level != BL_UDS_SEC_LEVEL) {
            return UDS_NRC_SubFunctionNotSupported;
        }
        if (a->len != sizeof(expect) || !g_seed_valid) {
            g_seed_valid = 0U;
            BL_SecNoteFail(&g_sec, UDSMillis());
            return UDS_NRC_InvalidKey;
        }
        g_seed_valid = 0U;                 /* one attempt per seed */
        bl_uds_key_from_seed(g_seed, expect);
        if (!BL_Sec_Equal(a->key, expect, sizeof(expect))) {
            BL_SecNoteFail(&g_sec, UDSMillis());
            return UDS_NRC_InvalidKey;
        }
        BL_SecNoteSuccess(&g_sec);
        return UDS_PositiveResponse;   /* library records the unlocked level */
    }

    case UDS_EVT_RoutineCtrl: {
        UDSRoutineCtrlArgs_t *a = (UDSRoutineCtrlArgs_t *)arg;
        /* the erase and check routines write or read the staging slot: programming + unlocked only */
        if (srv->sessionType != UDS_LEV_DS_PRGS || srv->securityLevel != BL_UDS_SEC_LEVEL) {
            return UDS_NRC_SecurityAccessDenied;
        }
        if (a->id == BL_UDS_RID_ERASE && a->ctrlType == UDS_LEV_RCTP_STR) {
            unsigned long pages = BL_UDS_DL_SIZE / BL_UDS_FLASH_PAGE;
            if (FlashIf_ErasePages(BL_UDS_DL_BASE, pages) != 1) {
                return UDS_NRC_GeneralProgrammingFailure;
            }
            return UDS_PositiveResponse;
        }
        if (a->id == BL_UDS_RID_CHECK && a->ctrlType == UDS_LEV_RCTP_STR) {
            /* CheckMemory: optionRecord = [addr:4][size:4]; reply with the CRC-32. */
            uint32_t addr, size, crc;
            uint8_t  rec[4];
            if (a->len < 8U) {
                return UDS_NRC_IncorrectMessageLengthOrInvalidFormat;
            }
            addr = ((uint32_t)a->optionRecord[0] << 24) | ((uint32_t)a->optionRecord[1] << 16) |
                   ((uint32_t)a->optionRecord[2] << 8)  |  (uint32_t)a->optionRecord[3];
            size = ((uint32_t)a->optionRecord[4] << 24) | ((uint32_t)a->optionRecord[5] << 16) |
                   ((uint32_t)a->optionRecord[6] << 8)  |  (uint32_t)a->optionRecord[7];
            if (addr < BL_UDS_DL_BASE || size == 0U ||
                (addr + size) > (BL_UDS_DL_BASE + BL_UDS_DL_SIZE)) {
                return UDS_NRC_RequestOutOfRange;
            }
            crc = bl_uds_crc32_region(addr, size);
            rec[0] = (uint8_t)(crc >> 24); rec[1] = (uint8_t)(crc >> 16);
            rec[2] = (uint8_t)(crc >> 8);  rec[3] = (uint8_t)(crc);
            a->copyStatusRecord(srv, rec, sizeof(rec));
            return UDS_PositiveResponse;
        }
        return UDS_NRC_RequestOutOfRange;
    }

    /* Accepted during programming to keep other traffic from interfering. */
    case UDS_EVT_CommCtrl:
    case UDS_EVT_ControlDTCSetting:
        return UDS_PositiveResponse;

    case UDS_EVT_RequestDownload: {
        UDSRequestDownloadArgs_t *a = (UDSRequestDownloadArgs_t *)arg;
        uint32_t addr = (uint32_t)(uintptr_t)a->addr;
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

    case UDS_EVT_ReadMemByAddr: {
        /* Read-back of the staging slot, so a client can verify a download. */
        UDSReadMemByAddrArgs_t *a = (UDSReadMemByAddrArgs_t *)arg;
        uint32_t addr = (uint32_t)(uintptr_t)a->memAddr;
        uint8_t  buf[BL_UDS_MAX_BLOCK];
        if (addr < BL_UDS_DL_BASE ||
            a->memSize == 0U ||
            a->memSize > BL_UDS_READ_MAX ||
            (addr + a->memSize) > g_dl_addr ||   /* only what this session downloaded */
            (addr + a->memSize) > (BL_UDS_DL_BASE + BL_UDS_DL_SIZE)) {
            return UDS_NRC_RequestOutOfRange;
        }
        FlashIf_Read(addr, buf, (unsigned long)a->memSize);
        a->copy(srv, buf, (uint16_t)a->memSize);
        return UDS_PositiveResponse;
    }

    case UDS_EVT_EcuReset: {
        UDSECUResetArgs_t *a = (UDSECUResetArgs_t *)arg;
        a->powerDownTimeMillis = 20U;  /* reset shortly after the reply is sent */
        return UDS_PositiveResponse;
    }

    case UDS_EVT_DoScheduledReset:
        bl_uds_system_reset();
        return UDS_PositiveResponse;

    /* Housekeeping notifications - no request to answer. */
    case UDS_EVT_SessionTimeout:
        bl_uds_forget_session_state();   /* the library has already dropped session and unlock */
        return UDS_PositiveResponse;

    case UDS_EVT_Err:
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
    isotp_init_link(&g_link_func, BL_UDS_ID_REPLY,
                    g_link_func_tx, sizeof(g_link_func_tx),
                    g_link_func_rx, sizeof(g_link_func_rx));
    g_tp.link[0]  = &g_link;
    g_tp.rx_id[0] = BL_UDS_ID_REQUEST;
    g_tp.link[1]  = &g_link_func;
    g_tp.rx_id[1] = BL_UDS_ID_FUNCTIONAL;

    UDSServerInit(&g_srv);
    BL_SecInit(&g_sec, UDSMillis());
    g_seed_valid = 0U;
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

/* 1-based index of the shared request that got a different answer, 0 when all matched */
volatile uint8_t bl_uds_vec_fail;

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

    /* The library answers SecurityAccess with 0x37 for its first second; wait that out so the
       checks below see the session rules and not the boot delay. */
    while ((int32_t)(UDSMillis() - g_srv.sec_access_boot_delay_timer) <= 0) {
        /* spin: ~1 s of real time on target; advances the tick on host */
    }

    /* The requests the production command layer must answer the same way
       (tests/vectors/uds_common.txt). bl_uds_vec_fail says which one differed. */
    for (uint32_t i = 0U; i < BL_UDS_VEC_COUNT; i++) {
        const bl_uds_vec_t *v = &bl_uds_vectors[i];
        n = bl_uds_tester_xfer(&tester, v->req, v->req_len, resp, sizeof(resp));
        if (n < v->exp_len || memcmp(resp, v->exp, v->exp_len) != 0) {
            bl_uds_vec_fail = (uint8_t)(i + 1U);
            rc = 30;
            goto done;
        }
    }

    /* 1) DiagnosticSessionControl: extended first, then programming. */
    req[0] = 0x10U;
    req[1] = UDS_LEV_DS_EXTDS;
    n = bl_uds_tester_xfer(&tester, req, 2U, resp, sizeof(resp));
    if (n == 0U) {
        rc = 1;   /* no response at all -> transport/server stalled */
        goto done;
    }
    if (n < 2U || resp[0] != 0x50U || resp[1] != UDS_LEV_DS_EXTDS) {
        rc = 2;   /* extended session not accepted */
        goto done;
    }
    req[0] = 0x10U;
    req[1] = UDS_LEV_DS_PRGS;
    n = bl_uds_tester_xfer(&tester, req, 2U, resp, sizeof(resp));
    if (n < 2U || resp[0] != 0x50U || resp[1] != UDS_LEV_DS_PRGS) {
        rc = 2;   /* programming session not accepted */
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

    /* 8) ReadMemoryByAddress: read the 8 bytes back and confirm they match.
          [0x23][ALFID=0x44][addr:4][size:4] -> [0x63][8 data bytes] */
    {
        static const uint8_t want[8] = {
            0xDEU, 0xADU, 0xBEU, 0xEFU, 0x11U, 0x22U, 0x33U, 0x44U
        };
        uint8_t rd[10] = {
            0x23U, 0x44U,
            (uint8_t)(BL_UDS_DL_BASE >> 24), (uint8_t)(BL_UDS_DL_BASE >> 16),
            (uint8_t)(BL_UDS_DL_BASE >> 8),  (uint8_t)(BL_UDS_DL_BASE),
            0x00U, 0x00U, 0x00U, 0x08U
        };
        n = bl_uds_tester_xfer(&tester, rd, sizeof(rd), resp, sizeof(resp));
        if (n < 9U || resp[0] != 0x63U || memcmp(&resp[1], want, 8) != 0) {
            rc = 9;   /* read-back did not match what was written */
            goto done;
        }
    }

    /* 9) RoutineControl CheckMemory: [0x31][start][0xFF 0x01][addr:4][size:4]
          -> [0x71][start][0xFF 0x01][crc:4]; the server's CRC must match ours. */
    {
        static const uint8_t payload[8] = {
            0xDEU, 0xADU, 0xBEU, 0xEFU, 0x11U, 0x22U, 0x33U, 0x44U
        };
        uint32_t exp = bl_uds_crc32_step(0xFFFFFFFFU, payload, 8U) ^ 0xFFFFFFFFU;
        uint32_t got;
        uint8_t  cm[12] = {
            0x31U, UDS_LEV_RCTP_STR, 0xFFU, 0x01U,
            (uint8_t)(BL_UDS_DL_BASE >> 24), (uint8_t)(BL_UDS_DL_BASE >> 16),
            (uint8_t)(BL_UDS_DL_BASE >> 8),  (uint8_t)(BL_UDS_DL_BASE),
            0x00U, 0x00U, 0x00U, 0x08U
        };
        n = bl_uds_tester_xfer(&tester, cm, sizeof(cm), resp, sizeof(resp));
        if (n < 8U || resp[0] != 0x71U || resp[2] != 0xFFU || resp[3] != 0x01U) {
            rc = 10;   /* CheckMemory routine rejected */
            goto done;
        }
        got = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
              ((uint32_t)resp[6] << 8)  |  (uint32_t)resp[7];
        if (got != exp) {
            rc = 10;   /* CheckMemory CRC did not match */
            goto done;
        }
    }

    rc = 0;   /* unlock + erase + download + exit + read-back + CheckMemory all passed */

done:
    BL_ISOTP_SwDisarm();
    return rc;
}
