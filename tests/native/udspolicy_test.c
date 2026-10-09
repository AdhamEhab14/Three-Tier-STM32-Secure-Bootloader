/*
 * Native unit test for the diagnostic session rules and service attributes (bl_udspolicy.c).
 *
 * Run it: tests/test_udspolicy.py builds and runs this for you (cd tests; python -m pytest -v test_udspolicy.py).
 * By hand, from the repository root, with a host gcc:
 *   gcc -O1 -Wall -Wextra -I STM32F103RBT6_Secure_Bootloader/Core/Inc tests/native/udspolicy_test.c STM32F103RBT6_Secure_Bootloader/Core/Src/bl_udspolicy.c -o udspolicy_test
 *   ./udspolicy_test
 */
#include <stdio.h>

#include "bl_udspolicy.h"

static int bad;

static void expect(const char *what, unsigned got, unsigned want)
{
    if (got != want) {
        printf("FAIL %s: got 0x%X, want 0x%X\n", what, got, want);
        bad++;
    } else {
        printf("ok   %s\n", what);
    }
}

int main(void)
{
    /* rows = from, columns = to, in the order default, programming, extended, safety */
    static const uint8_t sessions[4] = { BL_SESS_DEFAULT, BL_SESS_PROGRAMMING, BL_SESS_EXTENDED, BL_SESS_SAFETY };
    static const char *names[4] = { "default", "programming", "extended", "safety" };
    static const uint8_t allowed[4][4] = {
        /* to:    def prg ext saf */
        /* def */ { 1,  0,  1,  0 },
        /* prg */ { 1,  1,  0,  0 },
        /* ext */ { 1,  1,  1,  1 },
        /* saf */ { 1,  0,  0,  1 },
    };
    char label[64];
    unsigned f, t;

    for (f = 0; f < 4; f++) {
        for (t = 0; t < 4; t++) {
            snprintf(label, sizeof label, "%s -> %s", names[f], names[t]);
            expect(label, (unsigned)BL_UdsSessionChangeAllowed(sessions[f], sessions[t]), allowed[f][t]);
        }
    }
    expect("session 0 is not a session", (unsigned)BL_UdsSessionKnown(0x00), 0U);
    expect("session 5 is not a session", (unsigned)BL_UdsSessionKnown(0x05), 0U);
    expect("an unknown session can not be entered", (unsigned)BL_UdsSessionChangeAllowed(BL_SESS_EXTENDED, 0x05), 0U);

    /* gate: service, session, security level, addressing */
    expect("unknown service", BL_UdsGate(0x99, BL_SESS_DEFAULT, 0, BL_ADDR_PHYSICAL), 0x11U);
    expect("session control works in default", BL_UdsGate(0x10, BL_SESS_DEFAULT, 0, BL_ADDR_PHYSICAL), 0U);
    expect("SecurityAccess is refused in default", BL_UdsGate(0x27, BL_SESS_DEFAULT, 0, BL_ADDR_PHYSICAL), 0x7FU);
    expect("SecurityAccess works in extended", BL_UdsGate(0x27, BL_SESS_EXTENDED, 0, BL_ADDR_PHYSICAL), 0U);
    expect("SecurityAccess works in programming", BL_UdsGate(0x27, BL_SESS_PROGRAMMING, 0, BL_ADDR_PHYSICAL), 0U);
    expect("RequestDownload needs programming", BL_UdsGate(0x34, BL_SESS_EXTENDED, 1, BL_ADDR_PHYSICAL), 0x7FU);
    expect("RequestDownload needs security", BL_UdsGate(0x34, BL_SESS_PROGRAMMING, 0, BL_ADDR_PHYSICAL), 0x33U);
    expect("RequestDownload when unlocked", BL_UdsGate(0x34, BL_SESS_PROGRAMMING, 1, BL_ADDR_PHYSICAL), 0U);
    expect("TransferData needs security", BL_UdsGate(0x36, BL_SESS_PROGRAMMING, 0, BL_ADDR_PHYSICAL), 0x33U);
    expect("TesterPresent works functionally", BL_UdsGate(0x3E, BL_SESS_DEFAULT, 0, BL_ADDR_FUNCTIONAL), 0U);
    expect("session control works functionally", BL_UdsGate(0x10, BL_SESS_DEFAULT, 0, BL_ADDR_FUNCTIONAL), 0U);
    expect("SecurityAccess is physical only", BL_UdsGate(0x27, BL_SESS_EXTENDED, 0, BL_ADDR_FUNCTIONAL), 0x11U);
    expect("RequestDownload is physical only", BL_UdsGate(0x34, BL_SESS_PROGRAMMING, 1, BL_ADDR_FUNCTIONAL), 0x11U);
    expect("a session that does not exist", BL_UdsGate(0x22, 0x09, 0, BL_ADDR_PHYSICAL), 0x7FU);

    /* no service may be reachable in a session it cannot work in, and every row must
       accept at least one addressing mode */
    {
        unsigned sid, ok_rows = 0;
        for (sid = 0; sid < 256; sid++) {
            const bl_uds_service_t *s = BL_UdsService((uint8_t)sid);
            if (!s) continue;
            ok_rows++;
            if (s->address_mask == 0U || (s->address_mask & ~BL_ADDR_BOTH) != 0U ||
                s->session_mask == 0U || (s->session_mask & ~BL_SESS_ALL) != 0U) {
                printf("FAIL service 0x%02X has an empty or out-of-range mask\n", sid);
                bad++;
            }
        }
        expect("every service row is well formed", ok_rows > 0U, 1U);
    }

    return bad ? 1 : 0;
}
