/**
 ******************************************************************************
 * @file    bl_config.h
 * @author  Adham Ehab
 * @brief   Compile-time selection of the bootloader's CAN transport / diagnostic
 *          stack.
 *
 *          BL_USE_ISO_STACK == 0 : use the hand-rolled can_bl transport
 *                                  (CANTP_Send/Recv) - the historical default.
 *          BL_USE_ISO_STACK == 1 : use the standards stack (isotp-c for
 *                                  ISO 15765-2, iso14229 for ISO 14229-1 UDS).
 *
 *          The two stacks are mutually independent; the vendored library code
 *          is dead-stripped by the linker (--gc-sections) whenever nothing
 *          references it, so leaving this at 0 keeps the old binary unchanged.
 ******************************************************************************
 */
#ifndef BL_CONFIG_H_
#define BL_CONFIG_H_

#ifndef BL_USE_ISO_STACK
#define BL_USE_ISO_STACK   0
#endif

#endif /* BL_CONFIG_H_ */
