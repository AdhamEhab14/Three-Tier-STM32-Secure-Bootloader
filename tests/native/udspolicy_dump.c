/*
 * Prints every answer bl_udspolicy.c can give, one per line, so a test can compare them with
 * the Python model:
 *   S <from> <to> <allowed>                 the session changes
 *   G <sid> <session> <security> <addr> <nrc>   the service gate, for every combination
 */
#include <stdio.h>

#include "bl_udspolicy.h"

int main(void)
{
    unsigned f, t, sid, session, sec, addr;

    for (f = 0; f < 8; f++)
        for (t = 0; t < 8; t++)
            printf("S %u %u %d\n", f, t, BL_UdsSessionChangeAllowed((uint8_t)f, (uint8_t)t));

    for (sid = 0; sid < 256; sid++)
        for (session = 0; session < 6; session++)
            for (sec = 0; sec < 3; sec++)
                for (addr = 1; addr <= 2; addr++)
                    printf("G %u %u %u %u %u\n", sid, session, sec, addr,
                           BL_UdsGate((uint8_t)sid, (uint8_t)session, (uint8_t)sec, (uint8_t)addr));
    return 0;
}
