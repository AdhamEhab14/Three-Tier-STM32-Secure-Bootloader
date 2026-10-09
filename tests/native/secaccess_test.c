/*
 * Native unit test for the SecurityAccess brute-force policy (bl_secaccess.c).
 *
 * Run it: tests/test_seckey.py builds and runs this for you (cd tests; python -m pytest -v test_seckey.py).
 * By hand, from the repository root, with a host gcc:
 *   gcc -O1 -Wall -Wextra -I STM32F103RBT6_Secure_Bootloader/Core/Inc tests/native/secaccess_test.c STM32F103RBT6_Secure_Bootloader/Core/Src/bl_secaccess.c -o secaccess_test
 *   ./secaccess_test
 */
#include <stdio.h>

#include "bl_secaccess.h"

static int bad;

static void expect(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("FAIL %s: got 0x%X, want 0x%X\n", what, (unsigned)got, (unsigned)want);
        bad++;
    } else {
        printf("ok   %s\n", what);
    }
}

int main(void)
{
    bl_sec_t s;
    uint32_t t = 5000U;

    BL_SecInit(&s, t);
    expect("blocked during the boot delay", BL_SecGate(&s, t + 999U), BL_SEC_NRC_TIME_DELAY);
    expect("open after the boot delay", BL_SecGate(&s, t + 1000U), 0U);

    t += 1000U;
    BL_SecNoteFail(&s, t);
    expect("1st wrong key: short delay", BL_SecGate(&s, t + 500U), BL_SEC_NRC_EXCEEDED);
    expect("1st wrong key: open again after 1 s", BL_SecGate(&s, t + BL_SEC_FAIL_DELAY_MS), 0U);

    t += 1000U;
    BL_SecNoteFail(&s, t);
    expect("2nd wrong key: still the short delay", BL_SecFailDelayMs(&s), BL_SEC_FAIL_DELAY_MS);
    t += 1000U;
    BL_SecNoteFail(&s, t);
    expect("3rd wrong key: lockout", BL_SecFailDelayMs(&s), BL_SEC_LOCKOUT_MS);
    expect("locked at 9.9 s", BL_SecGate(&s, t + 9900U), BL_SEC_NRC_EXCEEDED);
    expect("open at 10 s", BL_SecGate(&s, t + BL_SEC_LOCKOUT_MS), 0U);

    t += BL_SEC_LOCKOUT_MS;
    BL_SecNoteFail(&s, t);
    expect("no free retry after a lockout", BL_SecFailDelayMs(&s), BL_SEC_LOCKOUT_MS);

    BL_SecNoteSuccess(&s);
    t += BL_SEC_LOCKOUT_MS;
    BL_SecNoteFail(&s, t);
    expect("a correct key resets the count", BL_SecFailDelayMs(&s), BL_SEC_FAIL_DELAY_MS);

    BL_SecInit(&s, 0xFFFFFF00U);   /* tick about to wrap */
    expect("wrap-around: blocked", BL_SecGate(&s, 0xFFFFFF10U), BL_SEC_NRC_TIME_DELAY);
    expect("wrap-around: open after the delay", BL_SecGate(&s, 0xFFFFFF00U + BL_SEC_BOOT_DELAY_MS), 0U);

    expect("seed counter starts at 1", BL_SecNextCounter(&s), 1U);
    expect("seed counter increases", BL_SecNextCounter(&s), 2U);

    return bad ? 1 : 0;
}
