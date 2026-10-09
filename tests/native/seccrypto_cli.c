/*
 * Native test and command-line tool for bl_seccrypto.c.
 *
 *   seccrypto_cli                     run the built-in vectors, exit 0 when all pass
 *   seccrypto_cli cmac <key> <msg>    print AES-CMAC (hex in, hex out; msg may be "-" for empty)
 *   seccrypto_cli keyforseed <seed>   print the SecurityAccess key for a 4-byte seed
 *   seccrypto_cli seed <ctr> <tick> <cyc>   print the seed BL_Sec_MakeSeed gives
 *
 * The CLI modes let pytest compare the firmware code with the Python implementation the
 * host tools use, on random inputs, not only on the published vectors.
 *
 * Run it: tests/test_seckey.py builds and runs this for you (cd tests; python -m pytest -v test_seckey.py).
 * By hand, from the repository root, with a host gcc:
 *   gcc -O1 -Wall -Wextra -Wno-cpp -I STM32F103RBT6_Secure_Bootloader/Core/Inc tests/native/seccrypto_cli.c STM32F103RBT6_Secure_Bootloader/Core/Src/bl_seccrypto.c -o seccrypto_cli
 *   ./seccrypto_cli
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bl_seccrypto.h"

static int hex2bin(const char *s, uint8_t *out, size_t max)
{
    size_t n = 0;
    if (strcmp(s, "-") == 0) return 0;
    while (s[0] && s[1] && n < max) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) return -1;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return (int)n;
}

static void print_hex(const uint8_t *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) printf("%02x", d[i]);
    printf("\n");
}

static int check(const char *name, const uint8_t *got, const uint8_t *want, size_t n)
{
    if (memcmp(got, want, n) != 0) {
        printf("FAIL %s\n  got  ", name);
        print_hex(got, n);
        printf("  want ");
        print_hex(want, n);
        return 1;
    }
    printf("ok   %s\n", name);
    return 0;
}

static int run_vectors(void)
{
    /* RFC 4493 section 4 */
    static const uint8_t key[16] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
    static const uint8_t msg[64] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
        0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
        0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
        0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10 };
    static const uint8_t m0[16]  = { 0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28, 0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46 };
    static const uint8_t m16[16] = { 0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44, 0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c };
    static const uint8_t m40[16] = { 0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30, 0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27 };
    static const uint8_t m64[16] = { 0x51, 0xf0, 0xbe, 0xbf, 0x7e, 0x3b, 0x9d, 0x92, 0xfc, 0x49, 0x74, 0x17, 0x79, 0x36, 0x3c, 0xfe };
    /* FIPS-197 appendix C.1 */
    static const uint8_t fkey[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
    static const uint8_t fpt[16]  = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    static const uint8_t fct[16]  = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    uint8_t out[16];
    int bad = 0;

    BL_Aes128Encrypt(fkey, fpt, out);       bad += check("AES-128 FIPS-197 C.1", out, fct, 16);
    BL_AesCmac(key, msg, 0, out);           bad += check("CMAC RFC4493 len 0", out, m0, 16);
    BL_AesCmac(key, msg, 16, out);          bad += check("CMAC RFC4493 len 16", out, m16, 16);
    BL_AesCmac(key, msg, 40, out);          bad += check("CMAC RFC4493 len 40", out, m40, 16);
    BL_AesCmac(key, msg, 64, out);          bad += check("CMAC RFC4493 len 64", out, m64, 16);

    {
        uint8_t a[4] = { 1, 2, 3, 4 }, b[4] = { 1, 2, 3, 4 }, c[4] = { 1, 2, 3, 5 };
        if (!BL_Sec_Equal(a, b, 4) || BL_Sec_Equal(a, c, 4)) { printf("FAIL BL_Sec_Equal\n"); bad++; }
        else printf("ok   BL_Sec_Equal\n");
    }
    {
        uint8_t s1[4], s2[4];
        BL_Sec_MakeSeed(s1, 1, 100, 5);
        BL_Sec_MakeSeed(s2, 2, 100, 5);
        if (memcmp(s1, s2, 4) == 0 || (s1[0] | s1[1] | s1[2] | s1[3]) == 0) { printf("FAIL seed\n"); bad++; }
        else printf("ok   seeds differ per counter and are never zero\n");
    }
    return bad;
}

int main(int argc, char **argv)
{
    uint8_t a[16], b[256], mac[16], key[4], seed[4];

    if (argc == 1) return run_vectors() ? 1 : 0;

    if (argc == 4 && strcmp(argv[1], "cmac") == 0) {
        int kl = hex2bin(argv[2], a, 16);
        int ml = hex2bin(argv[3], b, sizeof b);
        if (kl != 16 || ml < 0) return 2;
        BL_AesCmac(a, b, (uint32_t)ml, mac);
        print_hex(mac, 16);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "keyforseed") == 0) {
        if (hex2bin(argv[2], seed, 4) != 4) return 2;
        BL_Sec_KeyForSeed(seed, key);
        print_hex(key, 4);
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "seed") == 0) {
        BL_Sec_MakeSeed(seed, (uint32_t)strtoul(argv[2], NULL, 0), (uint32_t)strtoul(argv[3], NULL, 0),
                        (uint32_t)strtoul(argv[4], NULL, 0));
        print_hex(seed, 4);
        return 0;
    }
    fprintf(stderr, "usage: see the header comment\n");
    return 2;
}
