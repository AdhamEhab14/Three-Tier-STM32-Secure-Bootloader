"""ReadMemoryByAddress (0x23) on the production command layer: read back a download, nothing else."""
import pytest

import bl_host
import powercut as pc
import uds_helpers as uh
from renode_session import RenodeSerial


@pytest.fixture()
def unlocked(session):
    """Programming session, unlocked, a 16-byte download sitting in Slot B."""
    session.power_cycle(settle=0.0)
    ser = RenodeSerial(session, timeout=5)
    uh.wait_for_fbl(session, ser)
    uh.enter(ser, uh.SESSION_EXTENDED)
    uh.enter(ser, uh.SESSION_PROGRAMMING)
    assert bl_host.uds_unlock(ser)
    data = bytes(range(0xA0, 0xB0))
    assert uh.uds(ser, 0x34, 0x00, 0x44, *pc.SLOT_B.to_bytes(4, "big"), 0, 0, 0, 16)[0] == 0x74
    assert uh.uds(ser, 0x36, 0x01, *data)[:2] == bytes([0x76, 0x01])
    assert uh.uds(ser, 0x37)[0] == 0x77
    return ser, data


def read(ser, addr, size):
    return uh.uds(ser, 0x23, 0x44, *addr.to_bytes(4, "big"), *size.to_bytes(4, "big"))


def test_a_download_reads_back_as_written(unlocked):
    ser, data = unlocked
    assert read(ser, pc.SLOT_B, 16) == bytes([0x63]) + data


def test_reading_needs_the_unlock(session):
    session.power_cycle(settle=0.0)
    ser = RenodeSerial(session, timeout=5)
    uh.wait_for_fbl(session, ser)
    uh.enter(ser, uh.SESSION_EXTENDED)
    uh.enter(ser, uh.SESSION_PROGRAMMING)
    assert uh.nrc(read(ser, pc.SLOT_B, 16), 0x23) == uh.NRC_SECURITY_DENIED


@pytest.mark.parametrize("addr,size", [
    (0x08000000, 16),                    # the Boot Manager
    (pc.FBL_BASE, 16),                   # the bootloader, keys included
    (pc.SLOT_A, 16),                     # the application
    (pc.SLOT_B - 4, 8),                  # starts below staging
    (pc.SLOT_B + pc.FBL_SIZE - 8, 16),   # runs past its end
    (pc.SLOT_B, 65),                     # more than one reply carries
    (pc.SLOT_B, 0),
])
def test_only_the_staging_slot_can_be_read(unlocked, addr, size):
    ser, _ = unlocked
    assert uh.nrc(read(ser, addr, size), 0x23) == uh.NRC_OUT_OF_RANGE
