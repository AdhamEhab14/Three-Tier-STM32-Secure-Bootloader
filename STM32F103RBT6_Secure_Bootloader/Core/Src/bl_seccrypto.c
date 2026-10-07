/**
 * @file    bl_seccrypto.c
 * @brief   AES-128 encrypt, AES-CMAC (RFC 4493) and the SecurityAccess helpers.
 *
 * CMAC never decrypts, so only the encrypt direction is here: a 256-byte S-box and a key
 * schedule that is expanded per call. Slow but small, and a key is only used a few times.
 */
#include "bl_seccrypto.h"

/* Which SecurityAccess key gets built in */
#if defined(BL_TEST_KEYS)
#include "bl_test_seckey.h"          /* published test key, emulator and CI builds only */
#elif defined(BL_SEC_KEY_HEADER)
#include BL_SEC_KEY_HEADER           /* the product's own key, given on the build line */
#else
#include "bl_seckey_demo.h"          /* public demo key, fine for a demo and nothing else */
#endif

static const uint8_t sbox[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16
};

static uint8_t xtime(uint8_t x)
{
    return (uint8_t)((uint8_t)(x << 1) ^ (uint8_t)((x >> 7) * 0x1BU));
}

/* 11 round keys of 16 bytes each. */
static void key_expand(const uint8_t key[16], uint8_t rk[176])
{
    uint8_t rcon = 1U;
    uint32_t i;

    for (i = 0U; i < 16U; i++) rk[i] = key[i];
    for (i = 16U; i < 176U; i += 4U)
    {
        uint8_t t[4];
        t[0] = rk[i - 4U]; t[1] = rk[i - 3U]; t[2] = rk[i - 2U]; t[3] = rk[i - 1U];
        if ((i % 16U) == 0U)
        {
            uint8_t u = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[u];
            rcon = xtime(rcon);
        }
        rk[i]      = (uint8_t)(rk[i - 16U] ^ t[0]);
        rk[i + 1U] = (uint8_t)(rk[i - 15U] ^ t[1]);
        rk[i + 2U] = (uint8_t)(rk[i - 14U] ^ t[2]);
        rk[i + 3U] = (uint8_t)(rk[i - 13U] ^ t[3]);
    }
}

static void add_round_key(uint8_t s[16], const uint8_t *rk)
{
    uint32_t i;
    for (i = 0U; i < 16U; i++) s[i] ^= rk[i];
}

void BL_Aes128Encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t rk[176];
    uint8_t s[16];
    uint32_t round, i;

    key_expand(key, rk);
    for (i = 0U; i < 16U; i++) s[i] = in[i];
    add_round_key(s, rk);

    for (round = 1U; round <= 10U; round++)
    {
        uint8_t t[16];

        /* SubBytes + ShiftRows (state is column-major: s[4*col + row]) */
        for (i = 0U; i < 16U; i++)
        {
            uint32_t col = i / 4U, row = i % 4U;
            t[i] = sbox[s[4U * ((col + row) % 4U) + row]];
        }

        if (round != 10U)
        {
            /* MixColumns */
            for (i = 0U; i < 4U; i++)
            {
                uint8_t a0 = t[4U * i], a1 = t[4U * i + 1U], a2 = t[4U * i + 2U], a3 = t[4U * i + 3U];
                uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                s[4U * i]      = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
                s[4U * i + 1U] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
                s[4U * i + 2U] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
                s[4U * i + 3U] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        else
        {
            for (i = 0U; i < 16U; i++) s[i] = t[i];
        }
        add_round_key(s, &rk[16U * round]);
    }
    for (i = 0U; i < 16U; i++) out[i] = s[i];
}

/* Doubling in GF(2^128), RFC 4493 section 2.3 (used to derive the two CMAC subkeys). */
static void dbl(const uint8_t in[16], uint8_t out[16])
{
    uint8_t carry = (uint8_t)(in[0] >> 7);
    int i;

    for (i = 0; i < 15; i++) out[i] = (uint8_t)((uint8_t)(in[i] << 1) | (uint8_t)(in[i + 1] >> 7));
    out[15] = (uint8_t)((uint8_t)(in[15] << 1) ^ (uint8_t)(carry * 0x87U));
}

void BL_AesCmac(const uint8_t key[16], const uint8_t *msg, uint32_t len, uint8_t mac[16])
{
    uint8_t zero[16] = {0};
    uint8_t l[16], k1[16], k2[16], x[16], y[16], last[16];
    uint32_t n, i, j, base, rem;
    int complete;

    BL_Aes128Encrypt(key, zero, l);
    dbl(l, k1);
    dbl(k1, k2);

    n = (len + 15U) / 16U;
    if (n == 0U) { n = 1U; complete = 0; }
    else         { complete = ((len % 16U) == 0U); }

    for (i = 0U; i < 16U; i++) x[i] = 0U;

    for (i = 0U; i + 1U < n; i++)               /* every block except the last */
    {
        for (j = 0U; j < 16U; j++) y[j] = (uint8_t)(x[j] ^ msg[16U * i + j]);
        BL_Aes128Encrypt(key, y, x);
    }

    /* Last block: a full one is XORed with K1, a short one is padded with 0x80 00.. and
       XORed with K2. */
    base = 16U * (n - 1U);
    rem = len - base;
    for (j = 0U; j < 16U; j++)
    {
        if (j < rem)       last[j] = msg[base + j];
        else if (j == rem) last[j] = 0x80U;
        else               last[j] = 0x00U;
    }
    for (j = 0U; j < 16U; j++)
    {
        uint8_t m = complete ? msg[base + j] : last[j];
        y[j] = (uint8_t)(x[j] ^ m ^ (complete ? k1[j] : k2[j]));
    }
    BL_Aes128Encrypt(key, y, mac);
}

void BL_Sec_KeyForSeed(const uint8_t seed[4], uint8_t key[4])
{
    static const uint8_t k[BL_SEC_KEY_LEN] = BL_SEC_KEY_BYTES;
    uint8_t mac[16];
    uint32_t i;

    BL_AesCmac(k, seed, 4U, mac);
    for (i = 0U; i < 4U; i++) key[i] = mac[i];
}

void BL_Sec_MakeSeed(uint8_t seed[4], uint32_t counter, uint32_t tick, uint32_t cycles)
{
    static const uint8_t k[BL_SEC_KEY_LEN] = BL_SEC_KEY_BYTES;
    uint8_t in[12], mac[16];
    uint32_t i;

    for (i = 0U; i < 4U; i++)
    {
        in[i]      = (uint8_t)(counter >> (8U * i));
        in[4U + i] = (uint8_t)(tick    >> (8U * i));
        in[8U + i] = (uint8_t)(cycles  >> (8U * i));
    }
    BL_AesCmac(k, in, sizeof(in), mac);
    for (i = 0U; i < 4U; i++) seed[i] = mac[i];
    if ((seed[0] | seed[1] | seed[2] | seed[3]) == 0U) seed[0] = 0x01U;   /* never an all-zero seed */
}

int BL_Sec_Equal(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint8_t diff = 0U;
    uint32_t i;

    for (i = 0U; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return (diff == 0U) ? 1 : 0;
}
