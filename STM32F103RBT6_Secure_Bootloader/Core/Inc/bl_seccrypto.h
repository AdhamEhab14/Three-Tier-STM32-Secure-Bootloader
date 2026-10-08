/**
 * @file    bl_seccrypto.h
 * @brief   AES-128, AES-CMAC (RFC 4493) and the SecurityAccess key and seed helpers.
 *
 * Plain C with no HAL, so the same file builds for the target and for the PC tests that
 * check it against the RFC vectors.
 */
#ifndef BL_SECCRYPTO_H
#define BL_SECCRYPTO_H

#include <stdint.h>

#define BL_SEC_KEY_LEN   16U

/* One AES-128 block, encrypt only (FIPS-197). */
void BL_Aes128Encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/* AES-CMAC of msg[0..len) under a 128-bit key (RFC 4493). */
void BL_AesCmac(const uint8_t key[16], const uint8_t *msg, uint32_t len, uint8_t mac[16]);

/* The answer to a 4-byte seed: first 4 bytes of AES-CMAC(K, seed), with K the SecurityAccess
   key built into the firmware. */
void BL_Sec_KeyForSeed(const uint8_t seed[4], uint8_t key[4]);

/* A non-zero 4-byte seed: first 4 bytes of AES-CMAC(K, counter | tick | cycles). Without K
   it cannot be predicted. `counter` rises with every seed, so none repeats within a
   power-up; `tick` and `cycles` tell power-ups apart. */
void BL_Sec_MakeSeed(uint8_t seed[4], uint32_t counter, uint32_t tick, uint32_t cycles);

/* Compare without an early exit, so the time taken does not leak where bytes differ. 1 = equal. */
int BL_Sec_Equal(const uint8_t *a, const uint8_t *b, uint32_t n);

#endif /* BL_SECCRYPTO_H */
