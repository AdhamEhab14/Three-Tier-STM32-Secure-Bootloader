"""
ISO 14229 conformance suite for the bootloader's UDS server.

Each test states one rule the server must obey - a service that should be
accepted, or a malformed / out-of-turn request that must be refused with a
specific negative response code. Run it with:

    pytest --html=report.html --self-contained-html

against the in-process VirtualEcu, or later against the real board through a
bridge transport. The rules are lifted straight from Core/Src/bl_uds.c.
"""
from virtual_ecu import STAGING_BASE, STAGING_SIZE, SECRET


# ---- helpers ---------------------------------------------------------------

def assert_positive(resp, sid):
    """A positive reply echoes the service id + 0x40."""
    assert resp[0] == sid + 0x40, f"expected positive 0x{sid + 0x40:02X}, got {resp.hex(' ')}"


def assert_nrc(resp, sid, nrc):
    assert resp == bytes([0x7F, sid, nrc]), \
        f"expected NRC 0x{nrc:02X} for SID 0x{sid:02X}, got {resp.hex(' ')}"


def addr_bytes(addr):
    return list(addr.to_bytes(4, "big"))


def size_bytes(size):
    return list(size.to_bytes(4, "big"))


# ---- sessions --------------------------------------------------------------

def test_default_session_accepted(uds):
    assert_positive(uds.send([0x10, 0x01]), 0x10)


def test_programming_session_accepted(uds):
    assert_positive(uds.send([0x10, 0x02]), 0x10)


def test_extended_session_accepted(uds):
    assert_positive(uds.send([0x10, 0x03]), 0x10)


def test_unknown_session_subfunction_rejected(uds):
    assert_nrc(uds.send([0x10, 0x05]), 0x10, 0x12)


def test_unknown_service_rejected(uds):
    # 0xAA is not a service this build implements.
    assert_nrc(uds.send([0xAA]), 0xAA, 0x11)


# ---- security access -------------------------------------------------------

def test_seed_then_correct_key_unlocks(uds):
    seed = uds.send([0x27, 0x01])[2:6]
    key = bytes(s ^ k for s, k in zip(seed, SECRET))
    assert_positive(uds.send([0x27, 0x02, *key]), 0x27)


def test_wrong_key_rejected(uds):
    uds.send([0x27, 0x01])
    bad_key = [0x00, 0x00, 0x00, 0x00]
    assert_nrc(uds.send([0x27, 0x02, *bad_key]), 0x27, 0x35)


def test_key_wrong_length_rejected(uds):
    uds.send([0x27, 0x01])
    assert_nrc(uds.send([0x27, 0x02, 0x11, 0x22]), 0x27, 0x35)


def test_locks_after_repeated_bad_keys(uds):
    uds.send([0x27, 0x01])
    for _ in range(3):
        uds.send([0x27, 0x02, 0, 0, 0, 0])
    # Further attempts are refused outright until the delay expires.
    assert_nrc(uds.send([0x27, 0x02, 0, 0, 0, 0]), 0x27, 0x36)


# ---- security gate on the reprogramming services ---------------------------

def test_routine_control_needs_security(uds):
    uds.enter_programming()
    assert_nrc(uds.send([0x31, 0x01, 0xFF, 0x00]), 0x31, 0x33)


def test_request_download_needs_security(uds):
    uds.enter_programming()
    req = [0x34, 0x00, 0x44] + addr_bytes(STAGING_BASE) + size_bytes(8)
    assert_nrc(uds.send(req), 0x34, 0x33)


# ---- address-range validation ----------------------------------------------

def test_request_download_below_staging_rejected(uds):
    uds.unlock()
    below = STAGING_BASE - 0x1000
    req = [0x34, 0x00, 0x44] + addr_bytes(below) + size_bytes(8)
    assert_nrc(uds.send(req), 0x34, 0x31)


def test_request_download_past_staging_rejected(uds):
    uds.unlock()
    req = [0x34, 0x00, 0x44] + addr_bytes(STAGING_BASE) + size_bytes(STAGING_SIZE + 1)
    assert_nrc(uds.send(req), 0x34, 0x31)


def test_read_memory_out_of_range_rejected(uds):
    uds.unlock()
    req = [0x23, 0x44] + addr_bytes(STAGING_BASE - 4) + size_bytes(8)
    assert_nrc(uds.send(req), 0x23, 0x31)


def test_check_memory_incorrect_length_rejected(uds):
    uds.unlock()
    # CheckMemory needs an 8-byte addr+size record; give it two.
    assert_nrc(uds.send([0x31, 0x01, 0xFF, 0x01, 0x00, 0x00]), 0x31, 0x13)


# ---- services accepted to keep the bus quiet during programming ------------

def test_communication_control_accepted(uds):
    assert_positive(uds.send([0x28, 0x03, 0x01]), 0x28)


def test_control_dtc_setting_accepted(uds):
    assert_positive(uds.send([0x85, 0x02]), 0x85)


# ---- the headline: a full, verified reprogramming sequence -----------------

def test_full_reprogramming_sequence(uds):
    payload = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44])

    assert_positive(uds.unlock(), 0x27)

    # Erase the staging slot.
    assert uds.send([0x31, 0x01, 0xFF, 0x00]) == bytes([0x71, 0x01, 0xFF, 0x00])

    # RequestDownload -> reply carries maxNumberOfBlockLength = 128.
    req = [0x34, 0x00, 0x44] + addr_bytes(STAGING_BASE) + size_bytes(len(payload))
    assert uds.send(req) == bytes([0x74, 0x20, 0x00, 0x80])

    # Stream the block, then close the transfer.
    assert uds.send([0x36, 0x01, *payload]) == bytes([0x76, 0x01])
    assert uds.send([0x37]) == bytes([0x77])

    # Read it back and confirm the bytes actually landed.
    readback = uds.send([0x23, 0x44] + addr_bytes(STAGING_BASE) + size_bytes(len(payload)))
    assert readback == bytes([0x63]) + payload

    # CheckMemory must return the same CRC-32 the tester computes independently.
    from virtual_ecu import VirtualEcu
    expected_crc = VirtualEcu._crc32(payload)
    check = uds.send([0x31, 0x01, 0xFF, 0x01] + addr_bytes(STAGING_BASE) + size_bytes(len(payload)))
    assert check == bytes([0x71, 0x01, 0xFF, 0x01]) + expected_crc.to_bytes(4, "big")
