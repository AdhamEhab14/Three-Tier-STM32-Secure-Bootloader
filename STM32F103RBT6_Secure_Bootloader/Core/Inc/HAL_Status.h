/*
 * HAL_Status.h
 *
 *  Created on: Jul 12, 2026
 *      Author: Adham
 */

#ifndef COMMON_HAL_STATUS_H_
#define COMMON_HAL_STATUS_H_

/* ---------------------------------------- Data Type Declaration Start --------------------------- */

/* Generic status returned by the driver software interfaces */
typedef enum
{
	HAL_OK		= 0x00U,		// Operation completed successfully
	HAL_ERROR	= 0x01U,		// Operation failed (e.g. bad parameter)
	HAL_BUSY	= 0x02U,		// Resource is busy
	HAL_TIMEOUT	= 0x03U			// Operation timed out (e.g. clock never became ready)
}HAL_StatusType;

/* ---------------------------------------- Data Type Declaration End ----------------------------- */

#endif /* COMMON_HAL_STATUS_H_ */
