"""The iso14229 UDS server, run through its on-chip self-test.

The production bootloader does not link the standards-library server, but it has a
self-test build (cmake -DBL_UDS_SELFTEST=ON) that boots straight into a tester and a server
talking over a software ISO-TP loopback, checks the session rules and then runs the whole
reprogramming sequence. It reports on LD2: solid on = pass, N blinks = failure code N.
Here the board's LED pin is read in the emulator.
"""
import os

import pytest

from conftest import ROOT
from renode_session import RenodeSession, find_renode

BUILD = "build-udsself"
GPIOA_ODR = 0x4001080C
LD2 = 1 << 5


@pytest.fixture(scope="module")
def selftest_board():
    exe = find_renode()
    if not exe:
        pytest.skip("Renode not found (set RENODE_PATH)")
    if not os.path.exists(os.path.join(ROOT, BUILD, "fbl.elf")):
        pytest.skip("self-test firmware not built in %s" % BUILD)
    s = RenodeSession(exe, build_dir=BUILD)
    yield s
    s.close()


def test_the_server_passes_its_own_self_test(selftest_board):
    s = selftest_board
    s.idle(10.0)                                       # session rules, unlock, erase, download, read-back
    levels = []
    for _ in range(8):                                 # a failure code blinks every 0.5 s
        levels.append((s.read_word(GPIOA_ODR) & LD2) != 0)
        s.idle(0.3)
    assert all(levels), "LD2 is not solid on, so the self-test failed (levels seen: %s)%s" % (levels, which_vector(s))


def which_vector(s):
    """If the shared requests were the problem, say which one (the firmware keeps its number)."""
    import subprocess
    nm = subprocess.run(["arm-none-eabi-nm", os.path.join(ROOT, BUILD, "fbl.elf")],
                        capture_output=True, text=True).stdout
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "bl_uds_vec_fail":
            number = s.read_word(int(parts[0], 16)) & 0xFF
            return "; shared request number %d got a different answer than the production server" % number if number else ""
    return ""
