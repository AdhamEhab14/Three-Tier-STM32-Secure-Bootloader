/**
 ******************************************************************************
 * @file    bootloader.c
 * @author  Adham Ehab
 * @date    18/08/2026
 * @brief   Command bootloader. Command processing is transport-agnostic
 *          (BL_ProcessFrame); the transport (UART now, CAN too) just moves
 *          the frame buffers in and out.
 *
 * Frame:  [LEN][CMD][DATA...][CRC32 little-endian]   (LEN = bytes after it)
 * Reply:  ACK(0xCD) + len + payload,   or   NACK(0xAB)
 ******************************************************************************
 */
#include <string.h>     /* memcpy / memcmp */
#include "bootloader.h"
#include "usart.h"      /* huart2 */
#include "crc.h"        /* hcrc   */
#include "flash_if.h"   /* FlashIf_ErasePages / FlashIf_Write */
#include "tweetnacl.h"  /* crypto_hash + crypto_sign_open      */
#include "can_bl.h"     /* CAN driver + ISO-TP                  */

#define BL_HOST_UART   (&huart2)

#define BL_ACK         0xCDU
#define BL_NACK        0xABU

#define CBL_GET_VER_CMD      0x10U   /* read version                      */
#define CBL_GO_TO_ADDR_CMD   0x14U   /* launch the application            */
#define CBL_FLASH_ERASE_CMD  0x15U   /* erase pages                       */
#define CBL_MEM_WRITE_CMD    0x16U   /* write a chunk into a slot         */
#define CBL_VERIFY_CMD       0x17U   /* verify the staged image + install */
#define CBL_UPDATE_FBL       0x1BU   /* reprogram the bootloader itself   */
#define CBL_LOCK_BM          0x1CU   /* write-protect the Boot Manager    */
#define CBL_BIST             0x1DU   /* read the power-on self-test result */
#define CBL_UDS              0x20U   /* wraps a UDS (ISO 14229) request as its payload */

#define BL_VENDOR_ID   100U
#define BL_SW_MAJOR    1U
#define BL_SW_MINOR    5U
#define BL_SW_PATCH    0U

/* This FBL's own version, packed the same way as an image header's fw_version.
   A self-update is refused if the incoming FBL is older than this. */
#define FBL_VERSION_PACKED  (((uint32_t)BL_SW_MAJOR << 16) | ((uint32_t)BL_SW_MINOR << 8) | (uint32_t)BL_SW_PATCH)

/* Our public key. Firmware signed with the matching private key is trusted. */
static const uint8_t BL_PUBLIC_KEY[32] = {
    0x7F, 0xFB, 0xE9, 0xEC, 0xD0, 0x8D, 0xB6, 0x73, 0xA8, 0xC2, 0xBD, 0xCE, 0x3C, 0xC3, 0x36, 0x42,
    0x5F, 0xCA, 0x82, 0x6F, 0xA7, 0xB1, 0x0A, 0x17, 0x2B, 0x1D, 0xFD, 0xA1, 0x5C, 0x91, 0x14, 0x1F
};

/* Pre-shared ChaCha20 key. Firmware confidentiality only; the Ed25519 signature
   (over the staged ciphertext's hash) still provides authenticity + integrity.
   Must match host/keys/bl_enckey.bin. */
static const uint8_t BL_ENC_KEY[32] = {
    0x28, 0xF1, 0x1D, 0xFA, 0xA1, 0x72, 0x28, 0x9C,
    0x72, 0x1E, 0xF3, 0xF0, 0xD3, 0xB1, 0x98, 0xF6,
    0x4A, 0xE3, 0xE3, 0x8F, 0xE5, 0x5E, 0x1D, 0x6C,
    0x2C, 0x4F, 0x8A, 0x4F, 0x74, 0xD4, 0x06, 0xEA
};

#define BL_RX_MAX      256U
static uint8_t  bl_rx[BL_RX_MAX];        /* the current command frame      */
static uint8_t  bl_reply[BL_RX_MAX];     /* the reply the handler builds   */
static uint32_t bl_reply_len;
static uint8_t  bl_go_pending;           /* set by GO: jump after replying */
static uint8_t  bl_fbl_update_pending;   /* set by UPDATE_FBL: run the SBL after replying */
static uint8_t  bl_lock_bm_pending;      /* set by LOCK_BM: reload option bytes (reset) after replying */

/* ---- helpers ---- */
static uint32_t BL_ReadU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t BL_CRC_Bytes(const uint8_t *data, uint32_t len)
{
    uint32_t word, crc = 0U;
    __HAL_CRC_DR_RESET(&hcrc);
    for (uint32_t i = 0U; i < len; i++) {
        word = (uint32_t)data[i];
        crc  = HAL_CRC_Accumulate(&hcrc, &word, 1U);
    }
    return crc;
}

static uint32_t BL_CRC_Region(uint32_t addr, uint32_t len)
{
    return BL_CRC_Bytes((const uint8_t *)addr, len);
}

static int BL_VerifySignature(uint32_t img_addr, uint32_t img_size, const uint8_t *sig)
{
    uint8_t digest[64];
    uint8_t sm[BL_SIG_LEN + 64];
    uint8_t m[BL_SIG_LEN + 64];
    unsigned long long mlen = 0;
    uint32_t i;

    crypto_hash(digest, (const uint8_t *)img_addr, (unsigned long long)img_size);
    for (i = 0U; i < BL_SIG_LEN; i++) sm[i] = sig[i];
    for (i = 0U; i < 64U; i++)        sm[BL_SIG_LEN + i] = digest[i];

    return (crypto_sign_open(m, &mlen, sm, BL_SIG_LEN + 64U, BL_PUBLIC_KEY) == 0) ? 1 : 0;
}

/* ---- ChaCha20 (RFC 8439) - firmware confidentiality --------------------- */
#define ROTL32(v, n)  (((v) << (n)) | ((v) >> (32U - (n))))

static void ChaCha20_Block(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12], uint8_t out[64])
{
    static const uint32_t c[4] = { 0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U };
    uint32_t s[16], x[16];
    int i;

    s[0] = c[0]; s[1] = c[1]; s[2] = c[2]; s[3] = c[3];
    for (i = 0; i < 8; i++) s[4 + i] = BL_ReadU32(key + 4 * i);
    s[12] = counter;
    s[13] = BL_ReadU32(nonce + 0);
    s[14] = BL_ReadU32(nonce + 4);
    s[15] = BL_ReadU32(nonce + 8);

    for (i = 0; i < 16; i++) x[i] = s[i];

    for (i = 0; i < 10; i++) {   /* 20 rounds = 10 column+diagonal double-rounds */
        #define QR(a,b,cc,d) \
            x[a]+=x[b]; x[d]^=x[a]; x[d]=ROTL32(x[d],16); \
            x[cc]+=x[d]; x[b]^=x[cc]; x[b]=ROTL32(x[b],12); \
            x[a]+=x[b]; x[d]^=x[a]; x[d]=ROTL32(x[d],8);  \
            x[cc]+=x[d]; x[b]^=x[cc]; x[b]=ROTL32(x[b],7)
        QR(0,4,8,12);  QR(1,5,9,13);  QR(2,6,10,14); QR(3,7,11,15);
        QR(0,5,10,15); QR(1,6,11,12); QR(2,7,8,13);  QR(3,4,9,14);
        #undef QR
    }

    for (i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];
        out[4*i+0] = (uint8_t)(v);
        out[4*i+1] = (uint8_t)(v >> 8);
        out[4*i+2] = (uint8_t)(v >> 16);
        out[4*i+3] = (uint8_t)(v >> 24);
    }
}

/* Decrypt the ChaCha20 ciphertext staged in Slot B into Slot A (already erased),
   64 bytes per keystream block, so nothing large is held in RAM. Authenticity was
   already established by verifying the signature over the ciphertext's hash. */
static int BL_DecryptSlotBtoA(const img_header_t *hdr)
{
    const uint8_t *ct = (const uint8_t *)SLOT_B_BASE;
    uint8_t ks[64], pt[64];
    uint32_t off;

    for (off = 0U; off < hdr->payload_size; off += 64U) {
        uint32_t n = hdr->payload_size - off;
        uint32_t i;
        if (n > 64U) n = 64U;
        ChaCha20_Block(BL_ENC_KEY, off / 64U, hdr->nonce, ks);
        for (i = 0U; i < n; i++) pt[i] = ct[off + i] ^ ks[i];
        if (!FlashIf_Write(APP_BASE + off, pt, n)) return 0;
    }
    return 1;
}

int BootMgr_AppValid(void)
{
    const app_meta_t *meta = (const app_meta_t *)CONFIG_ADDR;

    if (meta->magic != APP_META_MAGIC)                 return 0;
    if (meta->size == 0U || meta->size > APP_MAX_SIZE) return 0;
    return (BL_CRC_Region(APP_BASE, meta->size) == meta->crc) ? 1 : 0;
}

/* Record our own CRC where the Boot Manager can check it (self-registration). */
void FBL_EnsureBmState(void)
{
    const bm_state_t *s = (const bm_state_t *)BM_STATE_ADDR;
    uint32_t crc = BL_CRC_Region(FBL_BASE_ADDR, FBL_REGION_SIZE);

    /* We booted, so this FBL is good: stamp the record VALID with our real CRC.
       This also clears any UPDATING flag left by a self-update, and refreshes a
       stale record after an ST-Link reflash. */
    if (s->magic != BM_STATE_MAGIC || s->crc != crc || s->state != BM_FBL_VALID) {
        bm_state_t ns;
        ns.magic = BM_STATE_MAGIC;
        ns.crc   = crc;
        ns.state = BM_FBL_VALID;
        FlashIf_ErasePages(BM_STATE_ADDR, 1U);
        FlashIf_Write(BM_STATE_ADDR, (const unsigned char *)&ns, sizeof(ns));
    }
}

/* ---- boot-trial record (anti-boot-loop) ---------------------------------
 * Layout in the BOOT_TRIAL_ADDR page (one half-word each):
 *   [0]   BOOT_TRIAL_ACTIVE - a trial is in progress
 *   [1]   0x0000            - the app confirmed itself (still 0xFFFF = not yet)
 *   [2..] one 0x0000 per recorded boot attempt (count of zeros = attempts)
 * Only bit-clears are needed after the initial erase, which is why a boot can
 * record an attempt without erasing the page.
 */
#define TRIAL_HW(i)  (*(volatile uint16_t *)(BOOT_TRIAL_ADDR + 2U * (uint32_t)(i)))

void BootTrial_Begin(void)
{
    uint16_t marker = BOOT_TRIAL_ACTIVE;
    FlashIf_ErasePages(BOOT_TRIAL_ADDR, 1U);
    FlashIf_Write(BOOT_TRIAL_ADDR, (const unsigned char *)&marker, sizeof(marker));
}

int BootTrial_AllowJump(void)
{
    uint32_t attempts = 0U;
    uint16_t zero = 0x0000U;

    if (TRIAL_HW(0) != BOOT_TRIAL_ACTIVE) return 1;   /* not on trial (ST-Link flash / legacy) */
    if (TRIAL_HW(1) == 0x0000U)           return 1;   /* app already confirmed itself */

    /* attempts are recorded in order from slot 2, so count the leading zeros */
    for (uint32_t i = 2U; i < 512U; i++) {
        if (TRIAL_HW(i) != 0x0000U) break;
        attempts++;
    }
    if (attempts >= BOOT_TRIAL_MAX) return 0;          /* boot-loop -> stay in recovery */

    /* record this boot attempt, then allow the launch */
    FlashIf_Write(BOOT_TRIAL_ADDR + 2U * (2U + attempts),
                  (const unsigned char *)&zero, sizeof(zero));
    return 1;
}

/* ---- built-in self-test (power-on) --------------------------------------
 * Three checks that the rest of the bootloader implicitly trusts:
 *   RAM   - a March C- pass over a scratch buffer (stuck-at / coupling faults)
 *   CRC   - the hardware CRC engine hashes a known vector to a known constant,
 *           and the installed app still matches its recorded CRC
 *   VDD   - the supply, measured against the 1.20 V internal reference
 * RAM and CRC are critical (their failure halts the boot); VDD is advisory.
 */
#define BIST_RAM_WORDS   256U               /* 1 KB scratch region under test */
#define BIST_CRC_VECTOR_EXPECT  0x798D8876U /* CRC of {DE,AD,BE,EF} in our scheme */

static bist_result_t g_bist;
static uint32_t bist_ram[BIST_RAM_WORDS];

static int BIST_RamMarch(void)
{
    const uint32_t Z = 0x00000000U, O = 0xFFFFFFFFU;
    uint32_t i;

    for (i = 0U; i < BIST_RAM_WORDS; i++) bist_ram[i] = Z;                              /* up   w0    */
    for (i = 0U; i < BIST_RAM_WORDS; i++) { if (bist_ram[i] != Z) return 0; bist_ram[i] = O; } /* up r0 w1 */
    for (i = 0U; i < BIST_RAM_WORDS; i++) { if (bist_ram[i] != O) return 0; bist_ram[i] = Z; } /* up r1 w0 */
    for (i = BIST_RAM_WORDS; i-- > 0U; )  { if (bist_ram[i] != Z) return 0; bist_ram[i] = O; } /* dn r0 w1 */
    for (i = BIST_RAM_WORDS; i-- > 0U; )  { if (bist_ram[i] != O) return 0; bist_ram[i] = Z; } /* dn r1 w0 */
    for (i = 0U; i < BIST_RAM_WORDS; i++)   if (bist_ram[i] != Z) return 0;             /* up   r0    */
    return 1;
}

static int BIST_CrcEngine(void)
{
    static const uint8_t vec[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    return (BL_CRC_Bytes(vec, 4U) == BIST_CRC_VECTOR_EXPECT) ? 1 : 0;
}

/* Read VDD via the internal 1.20 V reference on ADC1 channel 17, driven at the
   register level so the FBL needs no ADC HAL module. All waits are bounded so a
   dead ADC reports a failure instead of hanging the boot. */
static uint16_t BIST_ReadVdd_mv(int *ok)
{
    uint32_t raw, guard;
    *ok = 0;

    RCC->CFGR   &= ~(3U << 14);
    RCC->CFGR   |=  (2U << 14);          /* ADCPRE = /6 -> 12 MHz from 72 MHz PCLK2 */
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

    ADC1->CR2  = 0U;
    ADC1->CR2 |= ADC_CR2_TSVREFE;        /* enable the internal reference channel  */
    ADC1->CR2 |= ADC_CR2_ADON;           /* wake the ADC                           */
    for (guard = 0U; guard < 100000U; guard++) __NOP();   /* tSTAB settle           */

    ADC1->CR2 |= ADC_CR2_RSTCAL;
    for (guard = 0U; (ADC1->CR2 & ADC_CR2_RSTCAL) && guard < 100000U; guard++) {}
    ADC1->CR2 |= ADC_CR2_CAL;
    for (guard = 0U; (ADC1->CR2 & ADC_CR2_CAL) && guard < 100000U; guard++) {}

    ADC1->SMPR1 |= (7U << 21);           /* channel 17 sample time = 239.5 cycles   */
    ADC1->SQR1   = 0U;                    /* sequence length 1                       */
    ADC1->SQR3   = 17U;                   /* 1st (only) conversion = channel 17      */

    ADC1->CR2 |= ADC_CR2_EXTSEL;         /* regular trigger = SWSTART (111)         */
    ADC1->CR2 |= ADC_CR2_EXTTRIG;
    ADC1->CR2 |= ADC_CR2_SWSTART;        /* start the conversion                    */

    for (guard = 0U; !(ADC1->SR & ADC_SR_EOC) && guard < 200000U; guard++) {}
    raw = (ADC1->SR & ADC_SR_EOC) ? (ADC1->DR & 0xFFFFU) : 0U;

    ADC1->CR2 = 0U;                      /* power the ADC back down                 */
    RCC->APB2ENR &= ~RCC_APB2ENR_ADC1EN;

    if (raw == 0U) return 0U;
    *ok = 1;
    return (uint16_t)((1200U * 4095U) / raw);   /* VDD = Vrefint * full-scale / raw */
}

int BIST_Run(bist_result_t *out)
{
    const app_meta_t *meta = (const app_meta_t *)CONFIG_ADDR;
    int vok = 0;

    g_bist.ram_ok   = (uint8_t)BIST_RamMarch();
    g_bist.flash_ok = (uint8_t)(BIST_CrcEngine() &&
                                (meta->magic != APP_META_MAGIC || BootMgr_AppValid()));
    g_bist.vdd_mv   = BIST_ReadVdd_mv(&vok);
    g_bist.vdd_ok   = (uint8_t)(vok && g_bist.vdd_mv >= 2700U && g_bist.vdd_mv <= 3600U);

    if (out) *out = g_bist;
    return (g_bist.ram_ok && g_bist.flash_ok) ? 1 : 0;   /* VDD is advisory */
}

/* ---- reply builders (fill bl_reply instead of transmitting) ---- */
static void BL_ReplyData(uint8_t rlen, const uint8_t *payload)
{
    bl_reply[0] = BL_ACK;
    bl_reply[1] = rlen;
    for (uint8_t i = 0U; i < rlen; i++) bl_reply[2 + i] = payload[i];
    bl_reply_len = 2U + (uint32_t)rlen;
}
static void BL_ReplyByte(uint8_t b) { BL_ReplyData(1U, &b); }
static void BL_ReplyNACK(void)      { bl_reply[0] = BL_NACK; bl_reply_len = 1U; }

/* forward decl (used by the CAN round-trip self-test) */
static void BL_ProcessFrame(uint32_t total);

/* ---- command handlers ---- */
static void BL_Handle_GetVersion(void)
{
    uint8_t ver[4] = { BL_VENDOR_ID, BL_SW_MAJOR, BL_SW_MINOR, BL_SW_PATCH };
    BL_ReplyData(4U, ver);
}

static void BL_Handle_Erase(void)      /* [addr:4][num_pages:1] */
{
    uint32_t addr   = BL_ReadU32(&bl_rx[2]);
    uint32_t npages = bl_rx[6];
    BL_ReplyByte(FlashIf_ErasePages(addr, npages) ? 1U : 0U);
}

static void BL_Handle_Write(void)      /* [addr:4][len:1][data:len] */
{
    uint32_t addr = BL_ReadU32(&bl_rx[2]);
    uint8_t  len  = bl_rx[6];
    BL_ReplyByte(FlashIf_Write(addr, &bl_rx[7], len) ? 1U : 0U);
}

/*
 * Fully authenticate the image whose signed header sits at bl_rx[2..] against the
 * payload staged in Slot B. Payload layout on the wire: [header:88][signature:64].
 * On success the parsed header is copied into *out. Returns 1 only if:
 *   - magic + type match and the size is sane,
 *   - the staged bytes hash to the digest the header claims,
 *   - the header is signed by our key, and
 *   - the version is not older than `floor` (anti-rollback).
 */
static int BL_CheckImage(const uint8_t *buf, img_header_t *out, uint8_t expected_type, uint32_t floor)
{
    uint8_t digest[64];
    const uint8_t *sig = buf + IMG_HDR_SIZE;

    memcpy(out, buf, IMG_HDR_SIZE);   /* aligned copy of the header */

    /* app images land in Slot A (16 KB); FBL images can fill the whole 44 KB region */
    uint32_t max_size = (expected_type == IMG_TYPE_FBL) ? FBL_REGION_SIZE : APP_MAX_SIZE;

    if (out->magic != IMG_MAGIC)                                    return 0;
    if (out->img_type != expected_type)                            return 0;
    if (out->payload_size == 0U || out->payload_size > max_size)   return 0;

    /* the staged bytes must match the digest the header commits to */
    crypto_hash(digest, (const uint8_t *)SLOT_B_BASE, out->payload_size);
    if (memcmp(digest, out->digest, 64) != 0)                      return 0;

    /* the header itself must carry our signature */
    if (!BL_VerifySignature((uint32_t)buf, IMG_HDR_SIZE, sig))     return 0;

    /* anti-rollback: refuse anything older than what's already installed */
    if (out->fw_version < floor)                                   return 0;

    return 1;
}

/* Authenticate the app image whose signed header sits at `hdr_and_sig`
   ([header:100][signature:64]) against the payload staged in Slot B, then promote
   it into Slot A (decrypting if needed), record its metadata, and arm the boot
   trial. Shared by the classic VERIFY command and the UDS install routine.
   Returns 1 on success. */
static int BL_InstallApp(const uint8_t *hdr_and_sig)
{
    const app_meta_t *cur = (const app_meta_t *)CONFIG_ADDR;
    uint32_t floor = (cur->magic == APP_META_MAGIC && cur->version != 0xFFFFFFFFU)
                     ? cur->version : 0U;   /* installed version is the rollback floor */
    img_header_t hdr;
    uint32_t npages;
    int promoted;
    app_meta_t meta;

    if (!BL_CheckImage(hdr_and_sig, &hdr, IMG_TYPE_APP, floor)) return 0;

    npages = (hdr.payload_size + 1023U) / 1024U;
    /* Promote Slot B -> Slot A: decrypt if encrypted, else copy. Either way Slot A
       ends up holding the plaintext application. */
    promoted = FlashIf_ErasePages(APP_BASE, npages) &&
        ((hdr.flags & IMG_FLAG_ENCRYPTED)
            ? BL_DecryptSlotBtoA(&hdr)
            : FlashIf_Write(APP_BASE, (const unsigned char *)SLOT_B_BASE, hdr.payload_size));
    if (!promoted) return 0;

    meta.magic   = APP_META_MAGIC;
    meta.size    = hdr.payload_size;
    meta.crc     = BL_CRC_Region(APP_BASE, hdr.payload_size);
    meta.version = hdr.fw_version;
    if (FlashIf_ErasePages(CONFIG_ADDR, 1U) &&
        FlashIf_Write(CONFIG_ADDR, (const unsigned char *)&meta, sizeof(meta))) {
        BootTrial_Begin();   /* new app is on trial until it confirms itself */
        return 1;
    }
    return 0;
}

/* [header:100][signature:64] - the app image is staged in Slot B */
static void BL_Handle_Verify(void)
{
    BL_ReplyByte(BL_InstallApp(&bl_rx[2]) ? 1U : 0U);
}

/* Report the last power-on BIST result: [ram][flash][vdd_ok][vdd_lo][vdd_hi] */
static void BL_Handle_Bist(void)
{
    uint8_t p[5];
    p[0] = g_bist.ram_ok;
    p[1] = g_bist.flash_ok;
    p[2] = g_bist.vdd_ok;
    p[3] = (uint8_t)(g_bist.vdd_mv & 0xFFU);
    p[4] = (uint8_t)(g_bist.vdd_mv >> 8);
    BL_ReplyData(5U, p);
}

static void BL_Handle_Go(void)
{
    BL_ReplyByte(1U);
    bl_go_pending = 1U;   /* the transport loop jumps after sending this reply */
}

/* 0x1B: reprogram the FBL itself. The new FBL image is staged in Slot B as
 * [header:100][signature:64]. On success we run the SBL from RAM, which erases
 * the FBL region and copies the new image in, then resets. */
static void BL_Handle_UpdateFbl(void)
{
    img_header_t hdr;

    /* same checks as an app, but the image must be type FBL and no older than us */
    if (BL_CheckImage(&bl_rx[2], &hdr, IMG_TYPE_FBL, FBL_VERSION_PACKED))
    {
        /* Record the new FBL CRC and raise the UPDATING flag BEFORE the SBL runs.
           If power is lost mid-reprogram, the BM sees UPDATING + a CRC mismatch
           (partial FBL) and refuses to boot it. Slot B == what the SBL writes. */
        bm_state_t ns;
        ns.magic = BM_STATE_MAGIC;
        ns.crc   = BL_CRC_Region(SLOT_B_BASE, FBL_REGION_SIZE);
        ns.state = BM_FBL_UPDATING;
        FlashIf_ErasePages(BM_STATE_ADDR, 1U);
        FlashIf_Write(BM_STATE_ADDR, (const unsigned char *)&ns, sizeof(ns));

        BL_ReplyByte(1U);
        bl_fbl_update_pending = 1U;   /* BL_Run runs the SBL after sending this reply */
    }
    else
    {
        BL_ReplyByte(0U);
    }
}

/* Write-protect the Boot Manager (flash pages 0-15 = the 16 KB BM) via the
   option bytes. Enabling WRP erases+rewrites the option-byte block but the HAL
   preserves the current RDP level, so this does NOT read-lock the chip. After
   this, the BM cannot be erased or reprogrammed until WRP is removed with
   STM32CubeProgrammer. The reload/reset is deferred to BL_Run so the host still
   gets this ACK. */
static void BL_Handle_LockBm(void)
{
    FLASH_OBProgramInitTypeDef ob = {0};
    ob.OptionType = OPTIONBYTE_WRP;
    ob.WRPState   = OB_WRPSTATE_ENABLE;
    ob.WRPPage    = OB_WRP_PAGES0TO3 | OB_WRP_PAGES4TO7 |
                    OB_WRP_PAGES8TO11 | OB_WRP_PAGES12TO15;

    HAL_FLASH_Unlock();
    HAL_FLASH_OB_Unlock();

    if (HAL_FLASHEx_OBProgram(&ob) == HAL_OK)
    {
        BL_ReplyByte(1U);
        bl_lock_bm_pending = 1U;   /* BL_Run reloads the option bytes (reset) after replying */
    }
    else
    {
        HAL_FLASH_OB_Lock();
        HAL_FLASH_Lock();
        BL_ReplyByte(0U);
    }
}

/* ---- UDS (ISO 14229) service layer -------------------------------------------
 * Carried as the payload of CMD_UDS, so a real UDS reprogramming sequence works
 * over any transport and reuses the framing + crypto we already have. A request
 * is [SID][params]; a positive reply is [SID+0x40][data], a negative reply is
 * [0x7F][SID][NRC]. The flashing flow is the canonical one:
 *   0x10 programming session -> 0x27 seed/key unlock -> 0x34 request download ->
 *   0x36 transfer data (into Slot B) -> 0x37 exit -> 0x31 install routine (runs
 *   our signed verify + promote) -> 0x11 ECU reset.
 */
#define UDS_POS               0x40U
#define UDS_NEG               0x7FU
#define UDS_SESSION           0x10U
#define UDS_ECU_RESET         0x11U
#define UDS_RDBI              0x22U
#define UDS_SECURITY          0x27U
#define UDS_ROUTINE           0x31U
#define UDS_REQ_DOWNLOAD      0x34U
#define UDS_TRANSFER_DATA     0x36U
#define UDS_XFER_EXIT         0x37U
#define UDS_TESTER_PRESENT    0x3EU

#define NRC_SERVICE_NOT_SUPP  0x11U
#define NRC_SUBFUNC_NOT_SUPP  0x12U
#define NRC_INVALID_LENGTH    0x13U
#define NRC_SEQUENCE          0x24U
#define NRC_OUT_OF_RANGE      0x31U
#define NRC_SECURITY_DENIED   0x33U
#define NRC_INVALID_KEY       0x35U
#define NRC_PROG_FAILURE      0x72U

#define SESSION_DEFAULT       0x01U
#define SESSION_PROGRAMMING   0x02U
#define SESSION_EXTENDED      0x03U

#define UDS_KEY_SECRET        0x5A3C96E1U
#define UDS_MAX_BLOCK         128U       /* max TransferData payload */

static uint8_t  uds_session = SESSION_DEFAULT;
static uint8_t  uds_unlocked;
static uint32_t uds_seed;
static uint32_t uds_dl_addr;
static uint32_t uds_dl_remaining;
static uint8_t  uds_bsc;                 /* expected block sequence counter */
static uint8_t  bl_uds_reset_pending;

/* Demo seed->key transform. A real ECU keeps this secret; both ends share it. */
static uint32_t uds_key_from_seed(uint32_t seed)
{
    uint32_t k = (seed << 3) | (seed >> 29);   /* rotate left 3 */
    return k ^ UDS_KEY_SECRET;
}

static uint32_t uds_nrc(uint8_t *resp, uint8_t sid, uint8_t nrc)
{
    resp[0] = UDS_NEG; resp[1] = sid; resp[2] = nrc;
    return 3U;
}

static uint32_t UDS_Handle(const uint8_t *req, uint32_t len, uint8_t *resp)
{
    uint8_t sid;
    if (len < 1U) return 0U;
    sid = req[0];

    switch (sid)
    {
    case UDS_TESTER_PRESENT:
        if (len < 2U) return uds_nrc(resp, sid, NRC_INVALID_LENGTH);
        resp[0] = sid + UDS_POS; resp[1] = req[1] & 0x7FU;
        return 2U;

    case UDS_SESSION: {
        uint8_t sub = (len >= 2U) ? (req[1] & 0x7FU) : 0U;
        if (sub != SESSION_DEFAULT && sub != SESSION_PROGRAMMING && sub != SESSION_EXTENDED)
            return uds_nrc(resp, sid, NRC_SUBFUNC_NOT_SUPP);
        uds_session  = sub;
        uds_unlocked = 0U;                 /* a session change always re-locks */
        resp[0] = sid + UDS_POS; resp[1] = sub;
        resp[2] = 0x00; resp[3] = 0x32;    /* P2 = 50 ms    */
        resp[4] = 0x01; resp[5] = 0xF4;    /* P2* = 5000 ms */
        return 6U;
    }

    case UDS_SECURITY: {
        uint8_t sub = (len >= 2U) ? req[1] : 0U;
        if (sub == 0x01U) {                /* requestSeed */
            uds_seed = uds_unlocked ? 0U : ((HAL_GetTick() * 2654435761U) | 1U);
            resp[0] = sid + UDS_POS; resp[1] = sub;
            resp[2] = (uint8_t)(uds_seed >> 24); resp[3] = (uint8_t)(uds_seed >> 16);
            resp[4] = (uint8_t)(uds_seed >> 8);  resp[5] = (uint8_t)uds_seed;
            return 6U;
        }
        if (sub == 0x02U) {                /* sendKey */
            uint32_t key;
            if (len < 6U) return uds_nrc(resp, sid, NRC_INVALID_LENGTH);
            key = ((uint32_t)req[2] << 24) | ((uint32_t)req[3] << 16)
                | ((uint32_t)req[4] << 8) | req[5];
            if (uds_seed != 0U && key == uds_key_from_seed(uds_seed)) {
                uds_unlocked = 1U; uds_seed = 0U;
                resp[0] = sid + UDS_POS; resp[1] = sub;
                return 2U;
            }
            return uds_nrc(resp, sid, NRC_INVALID_KEY);
        }
        return uds_nrc(resp, sid, NRC_SUBFUNC_NOT_SUPP);
    }

    case UDS_RDBI: {
        uint16_t did;
        if (len < 3U) return uds_nrc(resp, sid, NRC_INVALID_LENGTH);
        did = ((uint16_t)req[1] << 8) | req[2];
        resp[0] = sid + UDS_POS; resp[1] = req[1]; resp[2] = req[2];
        switch (did) {
        case 0xF195U:   /* bootloader version */
            resp[3] = BL_VENDOR_ID; resp[4] = BL_SW_MAJOR; resp[5] = BL_SW_MINOR; resp[6] = BL_SW_PATCH;
            return 7U;
        case 0xF186U:   /* active diagnostic session */
            resp[3] = uds_session; return 4U;
        case 0xF190U: { /* installed app version */
            const app_meta_t *m = (const app_meta_t *)CONFIG_ADDR;
            uint32_t v = (m->magic == APP_META_MAGIC) ? m->version : 0U;
            resp[3] = (uint8_t)(v >> 16); resp[4] = (uint8_t)(v >> 8); resp[5] = (uint8_t)v;
            return 6U;
        }
        case 0xFD00U:   /* last BIST result */
            resp[3] = g_bist.ram_ok; resp[4] = g_bist.flash_ok; resp[5] = g_bist.vdd_ok;
            resp[6] = (uint8_t)g_bist.vdd_mv; resp[7] = (uint8_t)(g_bist.vdd_mv >> 8);
            return 8U;
        default:
            return uds_nrc(resp, sid, NRC_OUT_OF_RANGE);
        }
    }

    case UDS_REQ_DOWNLOAD: {
        /* [DFI][ALFID][addr:4][size:4]; we require ALFID = 0x44 */
        uint32_t addr, size, npages;
        if (uds_session != SESSION_PROGRAMMING || !uds_unlocked)
            return uds_nrc(resp, sid, NRC_SECURITY_DENIED);
        if (len < 11U || req[2] != 0x44U)
            return uds_nrc(resp, sid, NRC_INVALID_LENGTH);
        addr = ((uint32_t)req[3] << 24) | ((uint32_t)req[4] << 16) | ((uint32_t)req[5] << 8) | req[6];
        size = ((uint32_t)req[7] << 24) | ((uint32_t)req[8] << 16) | ((uint32_t)req[9] << 8) | req[10];
        if (addr != SLOT_B_BASE || size == 0U || size > FBL_REGION_SIZE)
            return uds_nrc(resp, sid, NRC_OUT_OF_RANGE);
        npages = (size + 1023U) / 1024U;
        if (!FlashIf_ErasePages(SLOT_B_BASE, npages))
            return uds_nrc(resp, sid, NRC_PROG_FAILURE);
        uds_dl_addr = SLOT_B_BASE; uds_dl_remaining = size; uds_bsc = 1U;
        resp[0] = sid + UDS_POS; resp[1] = 0x20;                    /* 2-byte maxBlockLength */
        resp[2] = (uint8_t)(UDS_MAX_BLOCK >> 8); resp[3] = (uint8_t)UDS_MAX_BLOCK;
        return 4U;
    }

    case UDS_TRANSFER_DATA: {
        uint8_t bsc; uint32_t dn;
        if (uds_dl_addr == 0U) return uds_nrc(resp, sid, NRC_SEQUENCE);
        if (len < 2U) return uds_nrc(resp, sid, NRC_INVALID_LENGTH);
        bsc = req[1];
        if (bsc == (uint8_t)(uds_bsc - 1U)) {        /* repeat of last block -> re-ack */
            resp[0] = sid + UDS_POS; resp[1] = bsc; return 2U;
        }
        if (bsc != uds_bsc) return uds_nrc(resp, sid, NRC_SEQUENCE);
        dn = len - 2U;
        if (dn > uds_dl_remaining) dn = uds_dl_remaining;
        if (dn > 0U && !FlashIf_Write(uds_dl_addr, &req[2], dn))
            return uds_nrc(resp, sid, NRC_PROG_FAILURE);
        uds_dl_addr += dn; uds_dl_remaining -= dn; uds_bsc++;
        resp[0] = sid + UDS_POS; resp[1] = bsc;
        return 2U;
    }

    case UDS_XFER_EXIT:
        uds_dl_addr = 0U; uds_dl_remaining = 0U;
        resp[0] = sid + UDS_POS;
        return 1U;

    case UDS_ROUTINE: {
        /* [sub][RID:2][data..]; startRoutine only */
        uint16_t rid;
        if (len < 4U) return uds_nrc(resp, sid, NRC_INVALID_LENGTH);
        if (req[1] != 0x01U) return uds_nrc(resp, sid, NRC_SUBFUNC_NOT_SUPP);
        rid = ((uint16_t)req[2] << 8) | req[3];
        if (rid == 0xFF01U) {              /* install: verify staged image + promote */
            if (uds_session != SESSION_PROGRAMMING || !uds_unlocked)
                return uds_nrc(resp, sid, NRC_SECURITY_DENIED);
            if (len < 4U + IMG_HDR_SIZE + BL_SIG_LEN)
                return uds_nrc(resp, sid, NRC_INVALID_LENGTH);
            resp[0] = sid + UDS_POS; resp[1] = 0x01; resp[2] = req[2]; resp[3] = req[3];
            resp[4] = BL_InstallApp(&req[4]) ? 1U : 0U;
            return 5U;
        }
        if (rid == 0x0202U) {              /* run the power-on self-test on demand */
            bist_result_t b;
            resp[0] = sid + UDS_POS; resp[1] = 0x01; resp[2] = req[2]; resp[3] = req[3];
            resp[4] = BIST_Run(&b) ? 1U : 0U;
            return 5U;
        }
        return uds_nrc(resp, sid, NRC_OUT_OF_RANGE);
    }

    case UDS_ECU_RESET: {
        uint8_t sub = (len >= 2U) ? req[1] : 0x01U;
        if (sub != 0x01U && sub != 0x03U) return uds_nrc(resp, sid, NRC_SUBFUNC_NOT_SUPP);
        resp[0] = sid + UDS_POS; resp[1] = sub;
        bl_uds_reset_pending = 1U;         /* BL_Run resets the MCU after replying */
        return 2U;
    }

    default:
        return uds_nrc(resp, sid, NRC_SERVICE_NOT_SUPP);
    }
}

static void BL_Handle_Uds(void)
{
    /* frame = [LEN][CMD_UDS][UDS PDU][CRC32]; the PDU length is LEN - 1 - 4 */
    uint8_t  udsresp[16 + IMG_HDR_SIZE];
    uint32_t rlen = UDS_Handle(&bl_rx[2], (uint32_t)bl_rx[0] - 5U, udsresp);
    if (rlen == 0U || rlen > 200U) { BL_ReplyNACK(); return; }
    BL_ReplyData((uint8_t)rlen, udsresp);
}

/* ---- transport-agnostic dispatch ---- */
static void BL_ProcessFrame(uint32_t total)
{
    uint32_t host_crc = BL_ReadU32(&bl_rx[total - 4]);
    if (BL_CRC_Bytes(bl_rx, total - 4U) != host_crc) {
        BL_ReplyNACK();
        return;
    }
    switch (bl_rx[1])
    {
        case CBL_GET_VER_CMD:     BL_Handle_GetVersion(); break;
        case CBL_FLASH_ERASE_CMD: BL_Handle_Erase();      break;
        case CBL_MEM_WRITE_CMD:   BL_Handle_Write();      break;
        case CBL_VERIFY_CMD:      BL_Handle_Verify();     break;
        case CBL_UPDATE_FBL:      BL_Handle_UpdateFbl();  break;
        case CBL_LOCK_BM:         BL_Handle_LockBm();     break;
        case CBL_BIST:            BL_Handle_Bist();       break;
        case CBL_UDS:             BL_Handle_Uds();        break;
        case CBL_GO_TO_ADDR_CMD:  BL_Handle_Go();         break;
        default:                  BL_ReplyNACK();         break;
    }
}

/* CAN IDs (CANBL_ID_CMD / CANBL_ID_REPLY) are defined in can_bl.h. */

/* USART1 (PA9/PA10, huart1) is the port the ESP32 WiFi gateway talks to. It's a
   normal CubeMX-configured peripheral (like USART2); we just poll it here with
   the same framed protocol, kept separate from the ST-Link VCP so both stay
   usable at once. huart1 comes from usart.h. */

/* ---- SPI2 slave (PB12 NSS, PB13 SCK, PB14 MISO, PB15 MOSI) + DATA_READY (PB1) --
 * The port the Blue Pill SPI bridge talks to. SPI is master-driven, so a plain
 * byte pipe can't work for slow commands (VERIFY runs Ed25519 for seconds): the
 * exchange is two phases with a DATA_READY handshake:
 *   1. the master clocks in a command frame (we read it here),
 *   2. we process it, raise DATA_READY, then stream the reply as the master clocks.
 * The pins, clock, and slave mode are configured by CubeMX (MX_SPI2_Init); the
 * frame/reply handling below just drives the registers, since HAL's fixed-length
 * slave API doesn't fit this variable-length, handshake-paced protocol. */
#define SPI_READY_HIGH()  (GPIOB->BSRR = (1U << 1))
#define SPI_READY_LOW()   (GPIOB->BSRR = (1U << (1 + 16)))

static void SPI2_SlaveInit(void)
{
    /* CubeMX muxes MISO (PB14) and enables the SPI2 clock, but it can only set
       *software* NSS, which leaves the slave permanently selected - stray clocks
       on the floating SCK then feed garbage into the poll loop and starve every
       transport. So we set hardware NSS here (slave selected only while the master
       pulls NSS/PB12 low) and configure the four pins: PB12 NSS input-pullup,
       PB13 SCK input, PB14 MISO AF push-pull, PB15 MOSI input. */
    GPIOB->CRH &= ~((0xFU << 16) | (0xFU << 20) | (0xFU << 24) | (0xFU << 28));
    GPIOB->CRH |=  ((0x8U << 16) | (0x4U << 20) | (0xBU << 24) | (0x4U << 28));
    GPIOB->ODR |= (1U << 12);          /* pull-up on NSS so it reads high when idle */

    SPI2->CR1 = 0U;                    /* slave, mode 0, 8-bit, MSB first, hardware NSS (SSM=0) */
    SPI2->CR2 = 0U;
    SPI2->CR1 |= SPI_CR1_SPE;          /* enable */
    SPI_READY_LOW();
}

/* Phase 1: if the master has clocked in a byte, read the whole command frame.
   Returns 1 and sets *total_out on a complete frame, else 0. */
static int SPI_SlavePoll(uint32_t *total_out)
{
    uint8_t len;
    uint32_t i, guard;

    /* Idle (NSS/PB12 high): drain any leftover byte and clear a stuck overrun so
       the next command starts from a clean RX. Without this a single overrun on a
       long transfer poisons every frame after it - the "works N times then wedges"
       failure. NSS is readable straight from IDR even in hardware-NSS slave mode. */
    if (GPIOB->IDR & (1U << 12)) {
        (void)SPI2->DR; (void)SPI2->SR;
        return 0;
    }

    if (!(SPI2->SR & SPI_SR_RXNE)) return 0;
    len = (uint8_t)SPI2->DR;
    if (len < 5U || (uint32_t)len + 1U > BL_RX_MAX) return 0;

    bl_rx[0] = len;
    for (i = 1U; i <= (uint32_t)len; i++) {
        guard = 0U;
        while (!(SPI2->SR & SPI_SR_RXNE)) {
            if (++guard > 3000000U) return 0;   /* mid-frame timeout */
        }
        bl_rx[i] = (uint8_t)SPI2->DR;
    }
    *total_out = (uint32_t)len + 1U;
    return 1;
}

/* Phase 2: stream the reply out as the master clocks, bracketed by DATA_READY. */
static void SPI_SlaveSendReply(const uint8_t *reply, uint32_t len)
{
    uint32_t i, guard;

    (void)SPI2->DR;                 /* clear any stale RX */
    SPI2->DR = reply[0];            /* preload so the first clocked byte is valid */
    SPI_READY_HIGH();               /* signal the master the reply is ready */

    for (i = 1U; i < len; i++) {
        guard = 0U;
        while (!(SPI2->SR & SPI_SR_TXE)) { if (++guard > 8000000U) goto done; }
        SPI2->DR = reply[i];
        if (SPI2->SR & SPI_SR_RXNE) (void)SPI2->DR;   /* drain RX so it can't overrun */
    }
    guard = 0U; while (!(SPI2->SR & SPI_SR_TXE)) { if (++guard > 8000000U) break; }
    guard = 0U; while (SPI2->SR & SPI_SR_BSY)   { if (++guard > 8000000U) break; }
done:
    SPI_READY_LOW();
    (void)SPI2->DR; (void)SPI2->SR;  /* clear OVR (read DR then SR) */
}

/* ---- I2C1 slave (PB6 SCL, PB7 SDA) at address 0x42 -------------------------
 * The other bus the Blue Pill bridge can talk on. Register-level (HAL's I2C
 * slave is errata-prone on the F103). Same two-phase DATA_READY handshake as
 * SPI: the master writes a command frame, we raise DATA_READY, the master reads
 * a fixed 8-byte reply. I2C clock-stretching holds the master while we service
 * each byte, so there's no tight timing to hit. */
#define I2C_ADDR7   0x42U

/* Reset the I2C peripheral logic and reconfigure it. Called at init and again
   after every exchange, so a stuck BUSY / held clock / leftover flag from one
   transaction can never carry into the next (the F103 "works once then hangs"). */
static void I2C_PeriphReset(void)
{
    I2C1->CR1  = I2C_CR1_SWRST;                 /* hold the peripheral in reset */
    I2C1->CR1  = 0U;                            /* release */
    I2C1->CR2  = 36U;                           /* APB1 = 36 MHz */
    I2C1->CCR  = 360U;                           /* 50 kHz (CCR/TRISE only matter for a master; harmless here) */
    I2C1->TRISE = 37U;
    I2C1->OAR1 = (uint16_t)((I2C_ADDR7 << 1) | (1U << 14));  /* bit14 must stay 1 */
    I2C1->CR1  = I2C_CR1_ACK | I2C_CR1_PE;      /* ACK our address + bytes, enable */
}

static void I2C1_SlaveInit(void)
{
    /* Pins and clock come from CubeMX (MX_I2C1_Init); we own the register setup
       via I2C_PeriphReset so init and the per-exchange reset stay identical. */
    I2C_PeriphReset();
}

/* Phase 1: if the master addressed us to WRITE, read the whole command frame. */
static int I2C_SlavePoll(uint32_t *total_out)
{
    uint32_t i, guard, sr2;
    uint8_t len;

    if (!(I2C1->SR1 & I2C_SR1_ADDR)) return 0;
    sr2 = I2C1->SR2;                    /* reading SR1 (above) + SR2 clears ADDR */
    if (sr2 & I2C_SR2_TRA) return 0;    /* a read, not a command - not our phase 1 */

    guard = 0U;
    while (!(I2C1->SR1 & I2C_SR1_RXNE)) { if (++guard > 3000000U) return 0; }
    len = (uint8_t)I2C1->DR;
    if (len < 5U || (uint32_t)len + 1U > BL_RX_MAX) return 0;
    bl_rx[0] = len;

    for (i = 1U; i <= (uint32_t)len; i++) {
        guard = 0U;
        while (!(I2C1->SR1 & I2C_SR1_RXNE)) {
            if (I2C1->SR1 & I2C_SR1_STOPF) { (void)I2C1->SR1; I2C1->CR1 |= I2C_CR1_PE; return 0; }
            if (++guard > 3000000U) return 0;
        }
        bl_rx[i] = (uint8_t)I2C1->DR;
    }

    /* swallow the closing STOP (read SR1 then write CR1 clears STOPF) */
    guard = 0U;
    while (!(I2C1->SR1 & I2C_SR1_STOPF)) {
        if (I2C1->SR1 & I2C_SR1_RXNE) (void)I2C1->DR;
        if (++guard > 3000000U) break;
    }
    (void)I2C1->SR1; I2C1->CR1 |= I2C_CR1_PE;
    *total_out = (uint32_t)len + 1U;
    return 1;
}

/* Phase 2: raise DATA_READY, wait for the master's READ, send a fixed 8-byte reply
   (the real reply padded with 0xFF). */
static void I2C_SlaveSendReply(const uint8_t *reply, uint32_t len)
{
    uint8_t out[8];
    uint32_t i, guard;

    for (i = 0U; i < 8U; i++) out[i] = (i < len) ? reply[i] : 0xFFU;

    I2C1->CR1 |= I2C_CR1_ACK;
    SPI_READY_HIGH();                  /* shared DATA_READY line (PB1) */

    guard = 0U;
    while (!(I2C1->SR1 & I2C_SR1_ADDR)) { if (++guard > 20000000U) goto done; }
    (void)I2C1->SR2;                   /* clear ADDR (transmit direction) */

    for (i = 0U; i < 8U; i++) {
        guard = 0U;
        while (!(I2C1->SR1 & (I2C_SR1_TXE | I2C_SR1_AF))) { if (++guard > 3000000U) goto fin; }
        if (I2C1->SR1 & I2C_SR1_AF) goto fin;
        I2C1->DR = out[i];
    }
fin:
done:
    /* Reset the peripheral so nothing (stuck BUSY, held SCL, leftover AF/STOPF)
       carries into the next transaction. This is what makes back-to-back I2C
       commands work instead of only the first after a power-on. */
    I2C_PeriphReset();
    SPI_READY_LOW();
}

/* ---- transport loop: USART2, USART1, CAN, SPI2, and I2C1 (all at once) ---- */
void BL_Run(void)
{
    CAN_BL_Init();   /* real CAN bus (CAN_BL_LOOPBACK = 0) + accept-all filter + start */
    SPI2_SlaveInit();/* SPI slave transport for the Blue Pill bridge */
    I2C1_SlaveInit();/* I2C slave transport for the Blue Pill bridge */

    while (1)
    {
        uint8_t  got = 0U;
        uint8_t  via_can = 0U;
        uint8_t  via_u1 = 0U;
        uint8_t  via_spi = 0U;
        uint8_t  via_i2c = 0U;
        uint32_t total = 0U;

        /* If a prior app started the IWDG, it survives the watchdog reset and
           is still counting. Refresh it here so recovery mode (this loop can
           wait indefinitely for a new upload) doesn't reset every ~2 s. Writing
           the refresh key is a no-op when the IWDG was never started. */
        IWDG->KR = 0xAAAAU;

        /* Drop a stale overrun so the next frame re-syncs from its start byte. */
        if (__HAL_UART_GET_FLAG(BL_HOST_UART, UART_FLAG_ORE)) __HAL_UART_CLEAR_OREFLAG(BL_HOST_UART);
        if (__HAL_UART_GET_FLAG(&huart1,      UART_FLAG_ORE)) __HAL_UART_CLEAR_OREFLAG(&huart1);

        /* SPI2 (Blue Pill bridge): checked first so a clocked-in frame is caught
           promptly rather than after the UART poll waits. */
        if (SPI_SlavePoll(&total))
        {
            got = 1U;
            via_spi = 1U;
        }

        /* I2C1 (Blue Pill bridge): clock-stretching holds the master, so latency
           here is harmless. */
        if (!got && I2C_SlavePoll(&total))
        {
            got = 1U;
            via_i2c = 1U;
        }

        /* Both UARTs are polled NON-blocking on the first byte. The F103 USART has
           only a 1-byte RX buffer (no FIFO), so blocking on an idle port would let
           a burst on the other port overrun and corrupt the frame. Once a start
           byte is seen, one continuous timed read pulls in the rest at line rate. */
        if (!got && __HAL_UART_GET_FLAG(BL_HOST_UART, UART_FLAG_RXNE) &&
            HAL_UART_Receive(BL_HOST_UART, &bl_rx[0], 1U, 2U) == HAL_OK)
        {
            uint8_t len = bl_rx[0];
            if (len >= 5U && ((uint32_t)len + 1U) <= BL_RX_MAX &&
                HAL_UART_Receive(BL_HOST_UART, &bl_rx[1], len, 50U) == HAL_OK)
            {
                total = (uint32_t)len + 1U;
                got = 1U;
            }
        }

        /* USART1: the ESP32 WiFi/BLE gateway - same non-blocking first byte. The
           inter-byte timeout is generous (250 ms) because the wireless links deliver
           a frame in bursts with gaps a tight timeout would trip on; a complete frame
           still returns the instant its last byte arrives. */
        if (!got && __HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) &&
            HAL_UART_Receive(&huart1, &bl_rx[0], 1U, 2U) == HAL_OK)
        {
            uint8_t len = bl_rx[0];
            if (len >= 5U && ((uint32_t)len + 1U) <= BL_RX_MAX &&
                HAL_UART_Receive(&huart1, &bl_rx[1], len, 250U) == HAL_OK)
            {
                total = (uint32_t)len + 1U;
                got = 1U;
                via_u1 = 1U;
            }
        }

        /* CAN: non-blocking - only if a full ISO-TP message is already waiting */
        if (!got)
        {
            uint32_t clen;
            if (CANTP_RecvNB(bl_rx, &clen, CANBL_ID_REPLY))
            {
                total = clen;
                got = 1U;
                via_can = 1U;
            }
        }

        if (!got) continue;

        bl_go_pending = 0U;
        bl_fbl_update_pending = 0U;
        bl_lock_bm_pending = 0U;
        bl_uds_reset_pending = 0U;
        BL_ProcessFrame(total);

        if (via_can)
            CANTP_Send(CANBL_ID_REPLY, bl_reply, bl_reply_len);   /* answer over CAN    */
        else if (via_u1)
            HAL_UART_Transmit(&huart1, bl_reply, bl_reply_len, HAL_MAX_DELAY);  /* answer to the ESP32 */
        else if (via_spi)
            SPI_SlaveSendReply(bl_reply, bl_reply_len);           /* answer to the Blue Pill SPI master */
        else if (via_i2c)
            I2C_SlaveSendReply(bl_reply, bl_reply_len);           /* answer to the Blue Pill I2C master */
        else
            HAL_UART_Transmit(BL_HOST_UART, bl_reply, bl_reply_len, HAL_MAX_DELAY);

        if (bl_go_pending) {
            HAL_Delay(2);
            BootMgr_JumpToApp();
        }

        if (bl_uds_reset_pending) {
            HAL_Delay(5);
            HAL_NVIC_SystemReset();    /* UDS ECUReset: reboot -> Boot Manager -> app */
        }

        if (bl_fbl_update_pending) {
            HAL_Delay(2);
            BootMgr_RunSBL();          /* SBL reprograms the FBL region, then resets */
        }

        if (bl_lock_bm_pending) {
            HAL_Delay(2);
            HAL_FLASH_OB_Launch();     /* reload option bytes -> system reset (never returns) */
        }
    }
}
