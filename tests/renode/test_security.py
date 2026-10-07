"""SecurityAccess (UDS 0x27) on the real firmware.

The key is the first 4 bytes of AES-CMAC(K, seed), a seed works for one attempt only, and
there is a boot delay, a wait after each wrong key and a 10 s lockout after three. The
answers come from the host's own seckey.py.
"""
import pytest

import bl_host
import seckey

NRC_INVALID_LENGTH = 0x13
NRC_INVALID_KEY = 0x35
NRC_EXCEEDED = 0x36
NRC_TIME_DELAY = 0x37

OLD_XOR_SECRET = bytes([0x19, 0x84, 0xC0, 0xDE])      # the scheme this replaced


def uds(ser, *pdu):
    ok, p = bl_host.transact(ser, bl_host.CMD_UDS, bytes(pdu))
    assert ok, "no reply to the UDS request"
    return bytes(p)


def nrc(resp):
    assert resp[0] == 0x7F and resp[1] == 0x27, "expected a negative response, got %s" % resp.hex()
    return resp[2]


def request_seed(ser):
    return uds(ser, 0x27, 0x01)


def send_key(ser, key):
    return uds(ser, 0x27, 0x02, *key)


def wait_for_fbl(session, ser, limit=6.0):
    """Run until the FBL answers a version request, then return straight away (the boot delay
    is still counting). The probe reply is consumed here so it cannot be mistaken for the next one."""
    reply = bytes([bl_host.ACK, 4, 100, 1, 5, 0])
    probe = bl_host.build_frame(bl_host.CMD_GET_VER)
    waited = 0.0
    while waited < limit:
        session.rx.clear()
        session.uart_write(probe)
        session.run_for(0.02)
        if bytes(session.rx).startswith(reply):
            session.rx.clear()
            return
        waited += 0.05
    raise AssertionError("the FBL never came up")


@pytest.fixture()
def fresh(session):
    """A freshly powered-up board with the gate still closed, and a serial handle."""
    from renode_session import RenodeSerial
    session.power_cycle(settle=0.0)
    ser = RenodeSerial(session, timeout=5)
    wait_for_fbl(session, ser)
    return session, ser


@pytest.fixture()
def open_gate(fresh):
    """The same, after the boot delay has passed."""
    session, ser = fresh
    session.idle(1.3)
    return session, ser


def test_security_access_is_refused_during_the_boot_delay(fresh):
    _, ser = fresh
    assert nrc(request_seed(ser)) == NRC_TIME_DELAY


def test_correct_key_unlocks(open_gate):
    session, ser = open_gate
    uds(ser, 0x10, 0x02)                              # programming session
    seed_resp = request_seed(ser)
    assert seed_resp[:2] == bytes([0x67, 0x01]) and len(seed_resp) == 6
    seed = seed_resp[2:6]
    assert seed != bytes(4)
    assert send_key(ser, seckey.key_for_seed(seed)) == bytes([0x67, 0x02])
    assert request_seed(ser)[2:6] == bytes(4), "an unlocked board should answer with an all-zero seed"


def test_the_old_xor_key_is_rejected(open_gate):
    _, ser = open_gate
    seed = request_seed(ser)[2:6]
    old_key = bytes(a ^ b for a, b in zip(seed, OLD_XOR_SECRET))
    assert nrc(send_key(ser, old_key)) == NRC_INVALID_KEY


def test_seeds_differ_on_every_request(open_gate):
    _, ser = open_gate
    seeds = [request_seed(ser)[2:6] for _ in range(6)]
    assert len(set(seeds)) == 6, "a seed repeated: %s" % [s.hex() for s in seeds]
    assert all(s != bytes(4) for s in seeds)


def test_a_key_of_the_wrong_length_is_refused(open_gate):
    _, ser = open_gate
    request_seed(ser)
    assert nrc(uds(ser, 0x27, 0x02, 1, 2, 3)) == NRC_INVALID_LENGTH


def test_a_seed_works_for_one_attempt_only(open_gate):
    session, ser = open_gate
    seed = request_seed(ser)[2:6]
    assert nrc(send_key(ser, b"\x00\x00\x00\x00")) == NRC_INVALID_KEY
    session.idle(1.2)                                 # out of the short delay
    assert nrc(send_key(ser, seckey.key_for_seed(seed))) == NRC_INVALID_KEY, \
        "the seed was still valid after a failed attempt"


def test_a_wrong_key_starts_a_delay_and_three_start_a_lockout(open_gate):
    session, ser = open_gate
    bad = b"\xDE\xAD\xBE\xEF"

    # 1st wrong key: short delay (1 s), then asking again works
    request_seed(ser)
    assert nrc(send_key(ser, bad)) == NRC_INVALID_KEY
    assert nrc(request_seed(ser)) == NRC_EXCEEDED, "no delay after a wrong key"
    session.idle(1.2)

    # 2nd and 3rd wrong keys
    for _ in range(2):
        assert request_seed(ser)[:2] == bytes([0x67, 0x01])
        assert nrc(send_key(ser, bad)) == NRC_INVALID_KEY
        session.idle(1.2)

    # after the 3rd the lockout is 10 s, not 1 s
    # the 3rd failure happened 1.2 s ago, so the board must still refuse
    assert nrc(request_seed(ser)) == NRC_EXCEEDED, "the lockout ended after only about 1 s"
    session.idle(8.0)                                 # about 9.2 s after the 3rd wrong key
    assert nrc(request_seed(ser)) == NRC_EXCEEDED, "the lockout ended before 10 s"
    session.idle(1.2)                                 # about 10.4 s
    seed_resp = request_seed(ser)
    assert seed_resp[:2] == bytes([0x67, 0x01]), "still locked out after 10 s"
    assert send_key(ser, seckey.key_for_seed(seed_resp[2:6])) == bytes([0x67, 0x02])


def test_the_host_tool_waits_out_the_boot_delay_by_itself(fresh):
    """bl_host.py asks for a seed right after power-up; it has to sit through the 0x37 and retry."""
    _, ser = fresh
    assert bl_host.uds_unlock(ser) is True
    assert request_seed(ser)[2:6] == bytes(4), "the board should now be unlocked"
