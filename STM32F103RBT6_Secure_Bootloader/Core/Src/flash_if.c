/**
 ******************************************************************************
 * @file    flash_if.c
 * @author  Adham Ehab
 * @date    18/08/2026
 * @brief   Implements flash_if.h on top of the bare-metal flash driver.
 ******************************************************************************
 */
#include "flash_if.h"
#include "flash.h"    /* your driver: FLASH_Unlock/ErasePage/ProgramHalfWord/Lock */

#define FLASH_PAGE   1024U

int FlashIf_ErasePages(unsigned long start_addr, unsigned long num_pages)
{
    if (FLASH_Unlock() != HAL_OK) {
        return 0;
    }
    for (unsigned long i = 0U; i < num_pages; i++) {
        if (FLASH_ErasePage((uint32_t)(start_addr + i * FLASH_PAGE)) != HAL_OK) {
            FLASH_Lock();
            return 0;
        }
    }
    FLASH_Lock();
    return 1;
}

int FlashIf_Write(unsigned long addr, const unsigned char *data, unsigned long len)
{
    if (FLASH_Unlock() != HAL_OK) {
        return 0;
    }
    for (unsigned long i = 0U; i < len; i += 2U) {
        uint16_t hw = (uint16_t)data[i];                    /* low byte  */
        if (i + 1U < len) {
            hw |= (uint16_t)((uint16_t)data[i + 1U] << 8);  /* high byte */
        } else {
            hw |= 0xFF00U;                                  /* pad odd last byte */
        }
        if (FLASH_ProgramHalfWord((uint32_t)(addr + i), hw) != HAL_OK) {
            FLASH_Lock();
            return 0;
        }
    }
    FLASH_Lock();
    return 1;
}

int FlashIf_Read(unsigned long addr, unsigned char *data, unsigned long len)
{
    const volatile unsigned char *src = (const volatile unsigned char *)addr;
    for (unsigned long i = 0U; i < len; i++) {
        data[i] = src[i];
    }
    return 1;
}
