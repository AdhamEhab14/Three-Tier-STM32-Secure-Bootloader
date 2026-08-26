/*
 * flash.h
 *
 *  Created on: Jul 12, 2026
 *      Author: Adham Ehab
 */

#ifndef FLASH_H_
#define FLASH_H_

/* ---------------------------------------- Includes Start ---------------------------------------- */

#include "Std_Types.h"
#include "HAL_Status.h"

/* ---------------------------------------- Includes End ------------------------------------------ */


/* ---------------------------------------- Macro Declaration Start ------------------------------- */

/* Memory map: FLASH interface registers (on the AHB, = 0x40020000 + 0x2000) */
#define FLASH_R_BASE				(0x40022000UL)
#define FLASH						((FLASH_Type*)FLASH_R_BASE)			// Pointer to the FLASH interface registers

/* Memory map: FLASH main memory (code) - STM32F103RB is a Medium-density device: 128 KB flash, 1 KB pages */
#define FLASH_BASE_ADDR				(0x08000000UL)							// First address of user flash
#define FLASH_SIZE					(0x00020000UL)							// 128 KB
#define FLASH_END_ADDR				(FLASH_BASE_ADDR + FLASH_SIZE - 1U)		// Last valid address (0x0801FFFF)
#define FLASH_PAGE_SIZE				(0x00000400UL)							// 1 KB per page (Medium-density devices)

/* FPEC unlock keys (fixed values defined by ST, RM0008) */
#define FLASH_KEY1					(0x45670123UL)
#define FLASH_KEY2					(0xCDEF89ABUL)


/* ******************* Bit definition for FLASH_ACR register ******************* */
#define FLASH_ACR_LATENCY_Pos		(0U)
#define FLASH_ACR_LATENCY_Msk		(0x7UL << FLASH_ACR_LATENCY_Pos)	/*!< 0x00000007 */
#define FLASH_ACR_HLFCYA_Pos		(3U)
#define FLASH_ACR_HLFCYA_Msk		(0x1UL << FLASH_ACR_HLFCYA_Pos)		/*!< 0x00000008 */
#define FLASH_ACR_PRFTBE_Pos		(4U)
#define FLASH_ACR_PRFTBE_Msk		(0x1UL << FLASH_ACR_PRFTBE_Pos)		/*!< 0x00000010 */
#define FLASH_ACR_PRFTBS_Pos		(5U)
#define FLASH_ACR_PRFTBS_Msk		(0x1UL << FLASH_ACR_PRFTBS_Pos)		/*!< 0x00000020 */

/* ******************* Flash wait-state (latency) options ******************* */
#define FLASH_LATENCY_0				(0x00000000U)						// 0 wait states,  0  < SYSCLK <= 24 MHz
#define FLASH_LATENCY_1				(0x00000001U)						// 1 wait state,   24 < SYSCLK <= 48 MHz
#define FLASH_LATENCY_2				(0x00000002U)						// 2 wait states,  48 < SYSCLK <= 72 MHz


/* ******************* Bit definition for FLASH_CR register ******************* */
#define FLASH_CR_PG_Pos				(0U)
#define FLASH_CR_PG_Msk				(0x1UL << FLASH_CR_PG_Pos)			/*!< 0x00000001 - Programming */
#define FLASH_CR_PER_Pos			(1U)
#define FLASH_CR_PER_Msk			(0x1UL << FLASH_CR_PER_Pos)			/*!< 0x00000002 - Page Erase */
#define FLASH_CR_MER_Pos			(2U)
#define FLASH_CR_MER_Msk			(0x1UL << FLASH_CR_MER_Pos)			/*!< 0x00000004 - Mass Erase */
#define FLASH_CR_STRT_Pos			(6U)
#define FLASH_CR_STRT_Msk			(0x1UL << FLASH_CR_STRT_Pos)			/*!< 0x00000040 - Start (erase trigger) */
#define FLASH_CR_LOCK_Pos			(7U)
#define FLASH_CR_LOCK_Msk			(0x1UL << FLASH_CR_LOCK_Pos)			/*!< 0x00000080 - Lock */

/* ******************* Bit definition for FLASH_SR register ******************* */
#define FLASH_SR_BSY_Pos			(0U)
#define FLASH_SR_BSY_Msk			(0x1UL << FLASH_SR_BSY_Pos)			/*!< 0x00000001 - Busy */
#define FLASH_SR_PGERR_Pos			(2U)
#define FLASH_SR_PGERR_Msk			(0x1UL << FLASH_SR_PGERR_Pos)		/*!< 0x00000004 - Programming Error */
#define FLASH_SR_WRPRTERR_Pos		(4U)
#define FLASH_SR_WRPRTERR_Msk		(0x1UL << FLASH_SR_WRPRTERR_Pos)	/*!< 0x00000010 - Write Protection Error */
#define FLASH_SR_EOP_Pos			(5U)
#define FLASH_SR_EOP_Msk			(0x1UL << FLASH_SR_EOP_Pos)			/*!< 0x00000020 - End Of operation */

/* ******************* Operation timeouts (software loop counts) ******************* */
#define FLASH_ERASE_TIMEOUT_VALUE	(0x000FFFFFU)						// Page/mass erase can take tens of ms
#define FLASH_PROGRAM_TIMEOUT_VALUE	(0x0000FFFFU)						// Half-word programming is much faster

/* ---------------------------------------- Macro Declaration End --------------------------------- */


/* ---------------------------------------- Data Type Declaration Start --------------------------- */

/* Creating a memory block for the FLASH interface registers */
typedef struct{
	volatile uint32_t ACR;				// Offset: 0x00 (R/W) Access Control Register
	volatile uint32_t KEYR;				// Offset: 0x04 (W)   FPEC Key Register
	volatile uint32_t OPTKEYR;			// Offset: 0x08 (W)   Option Byte Key Register
	volatile uint32_t SR;				// Offset: 0x0C (R/W) Status Register
	volatile uint32_t CR;				// Offset: 0x10 (R/W) Control Register
	volatile uint32_t AR;				// Offset: 0x14 (W)   Address Register
	uint32_t RESERVED;					// Offset: 0x18       Reserved
	volatile uint32_t OBR;				// Offset: 0x1C (R)   Option Byte Register
	volatile uint32_t WRPR;				// Offset: 0x20 (R)   Write Protection Register
}FLASH_Type;

/* ---------------------------------------- Data Type Declaration End ----------------------------- */


/* ---------------------------------------- Software Interfaces Declaration Start ----------------- */

/**
 * brief	Sets the flash access latency (wait states) and enables the prefetch buffer
 * details	The wait states must match the target HCLK: 0 WS up to 24 MHz, 1 WS up to 48 MHz, 2 WS up to 72 MHz.
 * 			Always raise the latency BEFORE increasing the clock, and lower it AFTER decreasing the clock.
 * param	Latency the number of wait states. A value of @ref FLASH_LATENCY_x
 */
void FLASH_SetLatency(uint32_t Latency);

/**
 * brief	Unlocks the FPEC controller so the flash CR register can be written
 * details	Writes the two fixed key values to FLASH_KEYR in sequence. Required once before any erase or
 * 			program operation; a reset (or FLASH_Lock) re-locks it.
 * return	HAL_OK    the controller is unlocked (LOCK bit clear)
 * return	HAL_ERROR the LOCK bit is still set after writing the keys (should not happen with correct keys)
 * note		Safe to call even if already unlocked - it is a no-op in that case
 */
HAL_StatusType FLASH_Unlock(void);

/**
 * brief	Re-locks the FPEC controller
 * details	Sets the LOCK bit in FLASH_CR. Call this after finishing a batch of erase/program operations
 * 			so the flash is not left writable by accident.
 */
void FLASH_Lock(void);

/**
 * brief	Erases one 1 KB flash page
 * details	Performs the full erase sequence: set PER, write the page address to AR, set STRT, then wait
 * 			for BSY to clear. Clears the EOP flag on success.
 * param	PageAddress any address inside the target page (e.g. 0x0800xxxx); does not need to be page-aligned
 * return	HAL_OK          the page was erased successfully
 * return	HAL_ERROR       PageAddress is outside the user flash range
 * return	HAL_TIMEOUT     BSY did not clear in time
 * return	HAL_BUSY        WRPRTERR set (the page is write-protected) - checked after the erase completes
 * note		FLASH_Unlock must be called first
 */
HAL_StatusType FLASH_ErasePage(uint32_t PageAddress);

/**
 * brief	Erases the entire user flash area
 * details	Performs the full mass-erase sequence: set MER, set STRT, then wait for BSY to clear
 * return	HAL_OK      the flash was erased successfully
 * return	HAL_TIMEOUT BSY did not clear in time
 * note		FLASH_Unlock must be called first. This erases the running application - only call it from
 * 			a bootloader context that is prepared to no longer have valid application code afterward.
 */
HAL_StatusType FLASH_MassErase(void);

/**
 * brief	Programs one 16-bit half-word into flash
 * details	Performs the program sequence: set PG, store Data at Address, then wait for BSY to clear.
 * 			Clears the EOP flag on success.
 * param	Address the flash address to program; must be within the user flash range and 2-byte aligned
 * param	Data the half-word value to write
 * return	HAL_OK       the half-word was programmed successfully
 * return	HAL_ERROR    Address is outside the user flash range or is not 2-byte aligned
 * return	HAL_TIMEOUT  BSY did not clear in time
 * return	HAL_BUSY     PGERR or WRPRTERR set (target not erased, or write-protected)
 * note		FLASH_Unlock must be called first, and the target address must already be erased (reads 0xFFFF)
 */
HAL_StatusType FLASH_ProgramHalfWord(uint32_t Address, uint16_t Data);

/**
 * brief	Programs one 32-bit word into flash
 * details	Convenience wrapper: programs Data as two consecutive half-words (low half-word first) via
 * 			FLASH_ProgramHalfWord
 * param	Address the flash address to program; must be within the user flash range and 4-byte aligned
 * param	Data the word value to write
 * return	HAL_OK       both half-words were programmed successfully
 * return	HAL_ERROR    Address is outside the user flash range or is not 4-byte aligned
 * return	HAL_TIMEOUT  a half-word program did not complete in time
 * return	HAL_BUSY     a half-word program reported PGERR or WRPRTERR
 * note		FLASH_Unlock must be called first, and the target address must already be erased
 */
HAL_StatusType FLASH_ProgramWord(uint32_t Address, uint32_t Data);

/* ---------------------------------------- Software Interfaces Declaration End ------------------- */

#endif /* FLASH_H_ */
