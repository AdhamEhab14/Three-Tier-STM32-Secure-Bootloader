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

/* Server event handler. Return UDS_PositiveResponse to accept a request (the
   library builds the positive reply), or a negative response code to reject. */
static UDSErr_t bl_uds_fn(UDSServer_t *srv, UDSEvent_t event, void *arg)
{
    (void)srv;

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
int BL_UDS_SelfTest(void)
{
    /* A tester link that plays the diagnostic client for one exchange. */
    IsoTpLink tester;
    uint8_t   tester_tx[64];
    uint8_t   tester_rx[64];

    IsoTpLink *links[2]  = { &g_link,           &tester };
    uint32_t   rx_ids[2] = { BL_UDS_ID_REQUEST, BL_UDS_ID_REPLY };
    /* server link receives requests on 0x7E0; tester receives replies on 0x7E8. */

    /* DiagnosticSessionControl -> programming session. */
    const uint8_t request[2] = { 0x10U, UDS_LEV_DS_PRGS };
    uint8_t  resp[64] = {0};
    uint32_t resp_len = 0;
    uint32_t guard;
    int rc;

    BL_UDS_Init();
    BL_ISOTP_InitLink(&tester, BL_UDS_ID_REQUEST, BL_UDS_ID_REPLY,
                      tester_tx, sizeof(tester_tx), tester_rx, sizeof(tester_rx));

    BL_ISOTP_SwArm(links, rx_ids, 2);

    rc = 0;
    if (isotp_send(&tester, request, sizeof(request)) != ISOTP_RET_OK) {
        rc = 1;   /* tester could not send the request */
        goto done;
    }

    rc = 2;   /* assume "no response" until one arrives */
    for (guard = 0; guard < 200000U; guard++) {
        BL_ISOTP_SwPump();       /* carry frames between the two links */
        UDSServerPoll(&g_srv);   /* let the server consume the request and answer */
        if (isotp_receive(&tester, resp, sizeof(resp), &resp_len) == ISOTP_RET_OK) {
            rc = 0;
            break;
        }
    }
    if (rc != 0) {
        goto done;
    }

    /* Expect the positive response: 0x50 <sessionType> <p2 hi><p2 lo>... */
    if (resp_len < 2U || resp[0] != 0x50U || resp[1] != UDS_LEV_DS_PRGS) {
        rc = 3;   /* not the expected positive session-control reply */
    }

done:
    BL_ISOTP_SwDisarm();
    return rc;
}
