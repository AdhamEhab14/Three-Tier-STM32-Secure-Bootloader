"""
Hardware conformance for the *production* UDS server (bootloader.c), driven over
the real CAN bus through the Blue Pill bridge.

This is the counterpart to test_uds_conformance.py: that one checks the standards
iso14229 server against a host model in CI; this one checks the shipping
hand-rolled server against the real board. The two servers differ (seed/key,
services), so this file encodes the production rules straight from
Core/Src/bootloader.c.

It only runs when you point it at a board:

    set HW_PORT=can:COM6         (through the Blue Pill bridge)   Windows
    set HW_PORT=COM3            (direct ST-Link UART, board in bootloader mode)
    pytest tests/test_uds_hardware.py -v

Without HW_PORT the whole module is skipped, so CI and the model suite are
unaffected. Every test is non-destructive: it only ever stages a few bytes into
the A/B staging slot and never runs the install routine, so the live app is
never touched.
"""
import os

import pytest

HW_PORT = os.environ.get("HW_PORT")
pytestmark = pytest.mark.skipif(
    not HW_PORT, reason="set HW_PORT (e.g. can:COM6) to run against the board")

UDS_KEY_SECRET = 0x5A3C96E1        # must match bootloader.c
SLOT_B_BASE = 0x08015000           # staging slot; RequestDownload only accepts this


def prod_key(seed):
    """key = rotate-left-3(seed) XOR secret - the production uds_key_from_seed()."""
    rotated = ((seed << 3) | (seed >> 29)) & 0xFFFFFFFF
    return rotated ^ UDS_KEY_SECRET


@pytest.fixture(scope="module")
def hw():
    from prod_bridge import ProdBridge
    bridge = ProdBridge(HW_PORT)
    yield bridge
    bridge.close()


# ---- helpers ---------------------------------------------------------------

def positive(resp, sid):
    assert resp and resp[0] == sid + 0x40, \
        f"expected positive 0x{sid + 0x40:02X}, got {resp.hex(' ') if resp else '(none)'}"


def nrc(resp, sid, code):
    assert resp[:3] == bytes([0x7F, sid, code]), \
        f"expected NRC 0x{code:02X} for 0x{sid:02X}, got {resp.hex(' ') if resp else '(none)'}"


def be(value, n):
    return list(value.to_bytes(n, "big"))


def unlock(hw):
    """Programming session + seed/key. A session change re-locks, so this always
    starts from a known state."""
    hw.request([0x10, 0x02])
    seed = int.from_bytes(hw.request([0x27, 0x01])[2:6], "big")
    return hw.request([0x27, 0x02, *be(prod_key(seed), 4)])


# ---- sessions & basics -----------------------------------------------------

def test_default_session(hw):
    positive(hw.request([0x10, 0x01]), 0x10)


def test_programming_session(hw):
    positive(hw.request([0x10, 0x02]), 0x10)


def test_bad_session_subfunction(hw):
    nrc(hw.request([0x10, 0x05]), 0x10, 0x12)


def test_tester_present(hw):
    positive(hw.request([0x3E, 0x00]), 0x3E)


def test_unknown_service(hw):
    nrc(hw.request([0xAA]), 0xAA, 0x11)


# ---- ReadDataByIdentifier --------------------------------------------------

def test_rdbi_bootloader_version(hw):
    r = hw.request([0x22, 0xF1, 0x95])
    assert r[:3] == bytes([0x62, 0xF1, 0x95]) and len(r) >= 7, r.hex(" ")


def test_rdbi_active_session(hw):
    hw.request([0x10, 0x01])                      # force default session first
    r = hw.request([0x22, 0xF1, 0x86])
    assert r[:3] == bytes([0x62, 0xF1, 0x86]) and r[3] == 0x01, r.hex(" ")


def test_rdbi_unknown_did(hw):
    nrc(hw.request([0x22, 0x12, 0x34]), 0x22, 0x31)


# ---- security access -------------------------------------------------------

def test_seed_key_unlocks(hw):
    positive(unlock(hw), 0x27)


def test_wrong_key_rejected(hw):
    hw.request([0x10, 0x02])
    hw.request([0x27, 0x01])
    nrc(hw.request([0x27, 0x02, 0x00, 0x00, 0x00, 0x00]), 0x27, 0x35)


def test_sendkey_wrong_length(hw):
    hw.request([0x10, 0x02])
    hw.request([0x27, 0x01])
    nrc(hw.request([0x27, 0x02, 0x11, 0x22]), 0x27, 0x13)


# ---- security gate & range checks on the download services -----------------

def test_download_requires_security(hw):
    hw.request([0x10, 0x02])                       # programming session, still locked
    req = [0x34, 0x00, 0x44] + be(SLOT_B_BASE, 4) + be(8, 4)
    nrc(hw.request(req), 0x34, 0x33)


def test_download_wrong_address_rejected(hw):
    unlock(hw)
    req = [0x34, 0x00, 0x44] + be(SLOT_B_BASE - 0x1000, 4) + be(8, 4)
    nrc(hw.request(req), 0x34, 0x31)


def test_download_and_transfer_into_staging(hw):
    # Non-destructive end-to-end: unlock, download + transfer a few bytes into the
    # staging slot, close the transfer. No install routine runs, so the live app
    # in Slot A is untouched.
    positive(unlock(hw), 0x27)
    payload = [0xDE, 0xAD, 0xBE, 0xEF]
    req = [0x34, 0x00, 0x44] + be(SLOT_B_BASE, 4) + be(len(payload), 4)
    positive(hw.request(req), 0x34)               # 0x74 ...
    positive(hw.request([0x36, 0x01, *payload]), 0x36)   # 0x76 01
    positive(hw.request([0x37]), 0x37)            # 0x77
