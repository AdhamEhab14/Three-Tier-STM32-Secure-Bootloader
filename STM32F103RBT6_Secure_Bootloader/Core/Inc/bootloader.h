/**
 ******************************************************************************
 * @file    bootloader.h
 * @author  Adham Ehab
 * @date    18/08/2026
 * @brief   UART command bootloader - public interface.
 ******************************************************************************
 */
#ifndef BOOTLOADER_H_
#define BOOTLOADER_H_

#include "main.h"   /* HAL types + peripheral handles */

/* Flash map (128 KB): BM 16K | FBL 40K | App(A) 28K | Slot B 40K | Config 4K.
   Slot B is >= the FBL region so a full new FBL can be staged for self-update. */
#define APP_BASE        0x0800E000U   /* application (Slot A) base            */
#define APP_MAX_SIZE    (28U * 1024U) /* size reserved for the application    */
#define SLOT_B_BASE     0x08015000U   /* staging slot: new image lands here first */
#define CONFIG_ADDR     0x0801FC00U   /* last 1 KB page: application metadata  */
#define APP_META_MAGIC  0x600DF00DU   /* "a valid app is present" marker       */
#define BL_SIG_LEN      64U           /* Ed25519 signature length              */

#define FBL_BASE_ADDR   0x08004000U   /* the FBL region (what the BM boots)   */
#define FBL_REGION_SIZE (40U * 1024U) /* whole FBL region                     */
#define BM_STATE_ADDR   0x0801F000U   /* config page holding the FBL CRC record */
#define BM_STATE_MAGIC  0xB007F00DU
#define BM_FBL_VALID    1U    /* FBL confirmed good (record may be trusted or stale) */
#define BM_FBL_UPDATING 2U    /* a self-update is in progress -> BM must verify strictly */

typedef struct { uint32_t magic; uint32_t crc; uint32_t state; } bm_state_t;

/*
 * Written after a signed image is verified at install time. At boot we only
 * re-check the fast CRC (the signature is the gate on the update channel, and
 * software Ed25519 is too slow to run on every power-on).
 */
typedef struct {
    uint32_t magic;   /* APP_META_MAGIC when a valid app is present */
    uint32_t size;    /* application image size in bytes            */
    uint32_t crc;     /* STM32 CRC over the application image        */
    uint32_t version; /* installed app version (anti-rollback floor) */
} app_meta_t;

/*
 * Signed image header. The host builds one of these for every image (app or FBL)
 * and signs it; the board checks the signature over the header, then checks the
 * payload staged in Slot B against the digest the (now-trusted) header claims.
 * Carrying the version inside the signed header is what makes anti-rollback safe.
 */
#define IMG_MAGIC     0x21474D49U   /* "IMG!" */
#define IMG_TYPE_APP  1U
#define IMG_TYPE_FBL  2U
#define IMG_HDR_SIZE  100U          /* bytes of img_header_t (before the 64-byte signature) */

#define IMG_FLAG_ENCRYPTED  0x0001U /* payload in Slot B is ChaCha20 ciphertext */
#define IMG_FLAG_COMPRESSED 0x0002U /* reserved for later (decompression)       */

typedef struct __attribute__((packed)) {
    uint32_t magic;         /* IMG_MAGIC                                       */
    uint8_t  img_type;      /* IMG_TYPE_APP / IMG_TYPE_FBL                     */
    uint8_t  hdr_version;   /* header format = 1                              */
    uint16_t flags;         /* IMG_FLAG_* bitmask                             */
    uint32_t fw_version;    /* (major<<16)|(minor<<8)|patch — anti-rollback    */
    uint32_t build_time;    /* unix epoch, informational                      */
    uint32_t payload_size;  /* bytes staged in Slot B (ciphertext == plaintext length) */
    uint32_t reserved;      /* padding / future                              */
    uint8_t  nonce[12];     /* ChaCha20 nonce (RFC 8439), used when encrypted  */
    uint8_t  digest[64];    /* SHA-512 of the payload exactly as staged in Slot B */
} img_header_t;

/*
 * Boot-trial / anti-boot-loop record (its own config page). A freshly installed
 * app is "on trial" until it proves itself: the FBL records each boot attempt,
 * the app programs the CONFIRMED half-word once it reaches a healthy state, and
 * after BOOT_TRIAL_MAX unconfirmed boots the FBL rolls back to recovery instead
 * of launching a bricked app forever. The page is programmed a half-word at a
 * time (erased only when a new trial is armed), so no wear per boot.
 */
#define BOOT_TRIAL_ADDR    0x0801F400U   /* dedicated config page for the trial record */
#define BOOT_TRIAL_ACTIVE  0xA5A5U       /* half-word[0]: an app is on trial            */
#define BOOT_TRIAL_MAX     3U            /* unconfirmed boots tolerated before recovery */

/*
 * Power-on built-in self-test result. RAM and the CRC engine are critical (a
 * failure halts the boot); VDD is advisory (reported, doesn't block booting).
 */
typedef struct {
    uint8_t  ram_ok;    /* March C- over a scratch RAM buffer passed        */
    uint8_t  flash_ok;  /* CRC engine known-vector + installed app integrity */
    uint8_t  vdd_ok;    /* measured supply within 2.7-3.6 V                 */
    uint16_t vdd_mv;    /* measured supply voltage in millivolts            */
} bist_result_t;

void BL_Run(void);                                 /* UART command loop (never returns) */
void BootTrial_Begin(void);                        /* arm the trial after a new app is installed */
int  BootTrial_AllowJump(void);                    /* 1 = ok to launch (records the attempt); 0 = boot-loop -> recovery */
int  BIST_Run(bist_result_t *out);                 /* 1 = critical self-tests passed; result stored for the BIST command */
void BootMgr_JumpToApp(void);                      /* launch app if valid (in main.c)   */
void BootMgr_RunSBL(void);                       /* copy the SBL into RAM and run it (in main.c) */
void FBL_EnsureBmState(void);                      /* record our own CRC for the Boot Manager */
int  BootMgr_AppValid(void);                       /* fast CRC re-check at boot          */

#endif /* BOOTLOADER_H_ */
