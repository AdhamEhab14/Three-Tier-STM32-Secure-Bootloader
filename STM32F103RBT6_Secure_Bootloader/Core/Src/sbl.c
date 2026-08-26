/**
 ******************************************************************************
 * @file    sbl.c
 * @author  Adham Ehab
 * @date    18/08/2026
 * @brief   Secondary Bootloader (SBL) / Bootloader Updater (BLU).
 *
 * Linked and executed from RAM (0x20001000). The FBL copies it into RAM and
 * jumps to it to perform a job the FBL cannot do to itself: erase and
 * reprogram the FBL flash region.
 *
 * Job: erase the FBL region (0x08004000, 40 KB) and copy the new FBL image
 * that was staged in Slot B (0x08015000) into it, then reset. The immutable
 * Boot Manager then boots the freshly written FBL.
 *
 * Bare-metal (no HAL) and self-contained: it carries its own flash routines,
 * because the FBL's flash driver lives in the very region being erased.
 ******************************************************************************
 */
#include <stdint.h>

#define SBL_FUNC __attribute__((section(".sbl_text"), used))

/* ---- FLASH controller registers (F103) ---- */
#define FLASH_KEYR   (*(volatile uint32_t *)0x40022004U)
#define FLASH_SR     (*(volatile uint32_t *)0x4002200CU)
#define FLASH_CR     (*(volatile uint32_t *)0x40022010U)
#define FLASH_AR     (*(volatile uint32_t *)0x40022014U)

#define FLASH_KEY1    0x45670123U
#define FLASH_KEY2    0xCDEF89ABU
#define FLASH_SR_BSY  (1U << 0)
#define FLASH_CR_PG   (1U << 0)
#define FLASH_CR_PER  (1U << 1)
#define FLASH_CR_STRT (1U << 6)
#define FLASH_CR_LOCK (1U << 7)

/* ---- system reset register ---- */
#define SCB_AIRCR    (*(volatile uint32_t *)0xE000ED0CU)

/* ---- fixed layout (the FBL update is always Slot B -> FBL region) ---- */
#define FBL_BASE     0x08004000U   /* destination: the FBL region  */
#define SLOT_B_BASE  0x08015000U   /* source: the staged new FBL   */
#define FBL_SIZE     0x0000A000U   /* 40 KB (whole FBL region)     */
#define FBL_PAGES    40U           /* 40 pages of 1 KB             */

SBL_FUNC static void sbl_flash_wait(void)
{
    while (FLASH_SR & FLASH_SR_BSY) { }
}

/* SBL entry point - runs entirely from RAM */
SBL_FUNC void sbl_main(void)
{
    uint32_t i;

    /* unlock the flash */
    FLASH_KEYR = FLASH_KEY1;
    FLASH_KEYR = FLASH_KEY2;

    /* erase the FBL region (FBL_PAGES pages of 1 KB) */
    for (i = 0U; i < FBL_PAGES; i++)
    {
        sbl_flash_wait();
        FLASH_CR |= FLASH_CR_PER;
        FLASH_AR  = FBL_BASE + i * 1024U;
        FLASH_CR |= FLASH_CR_STRT;
        sbl_flash_wait();
        FLASH_CR &= ~FLASH_CR_PER;
    }

    /* copy the new FBL image from Slot B into the FBL region (half-words) */
    for (i = 0U; i < FBL_SIZE; i += 2U)
    {
        uint16_t hw = *(volatile uint16_t *)(SLOT_B_BASE + i);   /* read from Slot B    */
        sbl_flash_wait();
        FLASH_CR |= FLASH_CR_PG;
        *(volatile uint16_t *)(FBL_BASE + i) = hw;               /* write to FBL region */
        sbl_flash_wait();
        FLASH_CR &= ~FLASH_CR_PG;
    }

    FLASH_CR |= FLASH_CR_LOCK;

    /* system reset -> the Boot Manager boots the freshly written FBL */
    SCB_AIRCR = (0x5FAU << 16) | (1U << 2);
    for (;;) { }
}

/* Minimal 2-entry vector table at the very start of the SBL image */
typedef void (*vector_entry_t)(void);

__attribute__((section(".sbl_vectors"), used))
const vector_entry_t sbl_vectors[2] = {
    (vector_entry_t)0x20005000U,   /* [0] initial MSP = top of 20 KB SRAM */
    sbl_main                       /* [1] reset vector                     */
};
