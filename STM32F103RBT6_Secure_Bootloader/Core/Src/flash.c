/*
 * flash.c
 *
 *  Created on: Jul 12, 2026
 *      Author: Adham Ehab
 */

#include "flash.h"


/**
 * brief	Sets the flash access latency (wait states) and enables the prefetch buffer
 * details	The wait states must match the target HCLK: 0 WS up to 24 MHz, 1 WS up to 48 MHz, 2 WS up to 72 MHz.
 * 			Always raise the latency BEFORE increasing the clock, and lower it AFTER decreasing the clock.
 * param	Latency the number of wait states. A value of @ref FLASH_LATENCY_x
 */
void FLASH_SetLatency(uint32_t Latency){
	/* Clear the LATENCY field, keep the prefetch buffer enabled, and write the new wait states in one go */
	FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY_Msk) | FLASH_ACR_PRFTBE_Msk | (Latency & FLASH_ACR_LATENCY_Msk);
}


/**
 * brief	Checks that an address falls inside the user flash range
 * param	Address the address to check
 * return	1 if Address is within [FLASH_BASE_ADDR, FLASH_END_ADDR], 0 otherwise
 */
static boolean FLASH_IsAddressValid(uint32_t Address){
	return ((Address >= FLASH_BASE_ADDR) && (Address <= FLASH_END_ADDR)) ? 1U : 0U;
}

/**
 * brief	Waits for the FPEC controller to finish its current operation and reports the result
 * details	Polls BSY until it clears or Timeout elapses, then checks/clears PGERR and WRPRTERR, and
 * 			clears EOP if it is set
 * param	Timeout software loop count to wait for BSY to clear
 * return	HAL_OK      the operation finished with no error flags set
 * return	HAL_TIMEOUT BSY did not clear within Timeout
 * return	HAL_BUSY    the operation finished but PGERR or WRPRTERR is set
 */
static HAL_StatusType FLASH_WaitForLastOperation(uint32_t Timeout){

	uint32_t timeout = Timeout;

	while((FLASH->SR & FLASH_SR_BSY_Msk) != 0U){
		if(timeout-- == 0U){
			return HAL_TIMEOUT;
		}
	}

	if((FLASH->SR & (FLASH_SR_PGERR_Msk | FLASH_SR_WRPRTERR_Msk)) != 0U){
		/* Error flags are cleared by writing 1 to them */
		FLASH->SR = (FLASH_SR_PGERR_Msk | FLASH_SR_WRPRTERR_Msk);
		return HAL_BUSY;
	}else{
		/* Do Nothing */
	}

	if((FLASH->SR & FLASH_SR_EOP_Msk) != 0U){
		FLASH->SR = FLASH_SR_EOP_Msk;			// Clear EOP by writing 1
	}else{
		/* Do Nothing */
	}

	return HAL_OK;
}

/**
 * brief	Unlocks the FPEC controller so the flash CR register can be written
 * details	Writes the two fixed key values to FLASH_KEYR in sequence. Required once before any erase or
 * 			program operation; a reset (or FLASH_Lock) re-locks it.
 * return	HAL_OK    the controller is unlocked (LOCK bit clear)
 * return	HAL_ERROR the LOCK bit is still set after writing the keys (should not happen with correct keys)
 * note		Safe to call even if already unlocked - it is a no-op in that case
 */
HAL_StatusType FLASH_Unlock(void){

	if((FLASH->CR & FLASH_CR_LOCK_Msk) != 0U){
		FLASH->KEYR = FLASH_KEY1;
		FLASH->KEYR = FLASH_KEY2;
	}else{
		/* Already unlocked - Do Nothing */
	}

	return ((FLASH->CR & FLASH_CR_LOCK_Msk) == 0U) ? HAL_OK : HAL_ERROR;
}

/**
 * brief	Re-locks the FPEC controller
 * details	Sets the LOCK bit in FLASH_CR. Call this after finishing a batch of erase/program operations
 * 			so the flash is not left writable by accident.
 */
void FLASH_Lock(void){
	FLASH->CR |= FLASH_CR_LOCK_Msk;
}

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
HAL_StatusType FLASH_ErasePage(uint32_t PageAddress){

	HAL_StatusType status = HAL_OK;

	if(FLASH_IsAddressValid(PageAddress) == 0U){
		return HAL_ERROR;
	}else{
		/* Do Nothing */
	}

	status = FLASH_WaitForLastOperation(FLASH_ERASE_TIMEOUT_VALUE);
	if(status != HAL_OK){
		return status;
	}else{
		/* Do Nothing */
	}

	FLASH->CR |= FLASH_CR_PER_Msk;
	FLASH->AR  = PageAddress;
	FLASH->CR |= FLASH_CR_STRT_Msk;

	status = FLASH_WaitForLastOperation(FLASH_ERASE_TIMEOUT_VALUE);

	FLASH->CR &= ~FLASH_CR_PER_Msk;

	return status;
}

/**
 * brief	Erases the entire user flash area
 * details	Performs the full mass-erase sequence: set MER, set STRT, then wait for BSY to clear
 * return	HAL_OK      the flash was erased successfully
 * return	HAL_TIMEOUT BSY did not clear in time
 * note		FLASH_Unlock must be called first. This erases the running application - only call it from
 * 			a bootloader context that is prepared to no longer have valid application code afterward.
 */
HAL_StatusType FLASH_MassErase(void){

	HAL_StatusType status = HAL_OK;

	status = FLASH_WaitForLastOperation(FLASH_ERASE_TIMEOUT_VALUE);
	if(status != HAL_OK){
		return status;
	}else{
		/* Do Nothing */
	}

	FLASH->CR |= FLASH_CR_MER_Msk;
	FLASH->CR |= FLASH_CR_STRT_Msk;

	status = FLASH_WaitForLastOperation(FLASH_ERASE_TIMEOUT_VALUE);

	FLASH->CR &= ~FLASH_CR_MER_Msk;

	return status;
}

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
HAL_StatusType FLASH_ProgramHalfWord(uint32_t Address, uint16_t Data){

	HAL_StatusType status = HAL_OK;

	if((FLASH_IsAddressValid(Address) == 0U) || ((Address & 0x1U) != 0U)){
		return HAL_ERROR;
	}else{
		/* Do Nothing */
	}

	status = FLASH_WaitForLastOperation(FLASH_PROGRAM_TIMEOUT_VALUE);
	if(status != HAL_OK){
		return status;
	}else{
		/* Do Nothing */
	}

	FLASH->CR |= FLASH_CR_PG_Msk;

	*(volatile uint16_t*)Address = Data;

	status = FLASH_WaitForLastOperation(FLASH_PROGRAM_TIMEOUT_VALUE);

	FLASH->CR &= ~FLASH_CR_PG_Msk;

	return status;
}

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
HAL_StatusType FLASH_ProgramWord(uint32_t Address, uint32_t Data){

	HAL_StatusType status = HAL_OK;

	if((FLASH_IsAddressValid(Address) == 0U) || ((Address & 0x3U) != 0U)){
		return HAL_ERROR;
	}else{
		/* Do Nothing */
	}

	status = FLASH_ProgramHalfWord(Address, (uint16_t)(Data & 0x0000FFFFU));
	if(status != HAL_OK){
		return status;
	}else{
		/* Do Nothing */
	}

	status = FLASH_ProgramHalfWord(Address + 2U, (uint16_t)((Data >> 16U) & 0x0000FFFFU));

	return status;
}
