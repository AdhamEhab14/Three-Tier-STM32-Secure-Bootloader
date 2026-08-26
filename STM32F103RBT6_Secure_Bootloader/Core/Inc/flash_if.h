/**
 ******************************************************************************
 * @file    flash_if.h
 * @author  Adham Ehab
 * @date    18/08/2026
 * @brief   Neutral glue between the HAL-side bootloader and the bare-metal
 *          flash driver. Uses only built-in C types, so it clashes with
 *          neither <stdint.h> (bootloader.c) nor Std_Types.h (flash.c).
 ******************************************************************************
 */
#ifndef FLASH_IF_H_
#define FLASH_IF_H_

/* Erase 'num_pages' 1 KB pages starting at start_addr. Returns 1 on success. */
int FlashIf_ErasePages(unsigned long start_addr, unsigned long num_pages);

/* Program 'len' bytes at addr (packed into 16-bit writes). Returns 1 on success. */
int FlashIf_Write(unsigned long addr, const unsigned char *data, unsigned long len);

#endif /* FLASH_IF_H_ */
