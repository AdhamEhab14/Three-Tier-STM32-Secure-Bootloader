"""The framed ERASE and WRITE commands may only touch the staging slot.

Images are authenticated when they are installed (VERIFY / the UDS install routine), so a
raw write anywhere else would let a host that can reach any transport put unsigned code in
the app slot, or damage the Boot Manager, the FBL or the metadata. Only Slot B is open to
the commands; everything else changes through a verified install.
"""
import contextlib
import struct

import pytest

import bl_host
import powercut as pc

SLOT_B_END = pc.SLOT_B + pc.FBL_SIZE          # 0x0801F000: the first byte after staging
MARK = 0x12345678

# one address in every region next to or outside staging
OUTSIDE = [
    0x08000000,          # Boot Manager
    pc.FBL_BASE,         # the bootloader itself
    pc.SLOT_A,           # the application
    pc.SLOT_B - 1024,    # the last page before staging
    SLOT_B_END,          # BM state, the first page after staging
    pc.BOOT_TRIAL,
    pc.CONFIG_ALT,
    pc.CONFIG,           # app metadata, the rollback floor
]


def erase(ser, addr, pages):
    ok, p = bl_host.transact(ser, bl_host.CMD_ERASE, struct.pack("<IB", addr, pages))
    return bl_host.ok_reply(ok, p)


def write(ser, addr, data, length=None):
    length = len(data) if length is None else length
    ok, p = bl_host.transact(ser, bl_host.CMD_WRITE, struct.pack("<I", addr) + bytes([length]) + data)
    return bl_host.ok_reply(ok, p)


@contextlib.contextmanager
def word_set(session, addr, value):
    """Put `value` at addr for the duration of a test, then put the old contents back."""
    old = session.read_word(addr)
    session.cmd("sysbus WriteDoubleWord 0x%X 0x%X" % (addr, value))
    try:
        yield
    finally:
        session.cmd("sysbus WriteDoubleWord 0x%X 0x%X" % (addr, old))


@pytest.mark.parametrize("addr", OUTSIDE, ids=lambda a: "0x%08X" % a)
def test_erase_outside_staging_is_refused(session, ser, addr):
    with word_set(session, addr, MARK):
        assert not erase(ser, addr, 1), "ERASE was accepted"
        assert session.read_word(addr) == MARK, "the page was erased"


@pytest.mark.parametrize("addr", OUTSIDE, ids=lambda a: "0x%08X" % a)
def test_write_outside_staging_is_refused(session, ser, addr):
    with word_set(session, addr, 0xFFFFFFFF):          # erased, so a write could land
        assert not write(ser, addr, b"\xAA\xBB\xCC\xDD"), "WRITE was accepted"
        assert session.read_word(addr) == 0xFFFFFFFF, "something was written"


def test_erase_may_not_run_past_the_end_of_staging(session, ser):
    with word_set(session, SLOT_B_END, MARK):
        assert not erase(ser, pc.SLOT_B, 41)           # one page more than staging holds
        assert session.read_word(SLOT_B_END) == MARK


def test_write_may_not_straddle_the_end_of_staging(session, ser):
    with word_set(session, SLOT_B_END, 0xFFFFFFFF):
        assert not write(ser, SLOT_B_END - 2, b"\x11\x22\x33\x44")
        assert session.read_word(SLOT_B_END) == 0xFFFFFFFF


def test_a_write_longer_than_its_frame_is_refused(session, ser):
    """The length byte claims more data than the frame carries."""
    assert not write(ser, pc.SLOT_B, b"\x01\x02\x03\x04", length=40)


def test_an_odd_write_address_is_refused(session, ser):
    assert erase(ser, pc.SLOT_B, 1)
    assert not write(ser, pc.SLOT_B + 1, b"\x01\x02")


def test_staging_itself_is_still_open(session, ser):
    assert erase(ser, pc.SLOT_B, 2)
    assert write(ser, pc.SLOT_B, bytes(range(16)))
    assert erase(ser, SLOT_B_END - 1024, 1)            # the last page of staging
    assert write(ser, SLOT_B_END - 4, b"\x01\x02\x03\x04")
    assert session.read_word(SLOT_B_END - 4) == 0x04030201
