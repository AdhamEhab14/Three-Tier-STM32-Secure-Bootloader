"""
Robustness / fuzz tests for the UDS server (ISO 21434 flavour).

A diagnostic server sits on a bus anyone can write to, so it has to survive
malformed traffic: random bytes, truncated PDUs, wrong lengths, garbage
sub-functions. These tests throw thousands of such requests at the server and
assert two things every time:

  1. it never crashes, and every reply is a well-formed UDS response - either a
     positive reply (SID + 0x40) or a three-byte negative response with a known
     NRC;
  2. it never leaks privilege - the reprogramming services (erase, download,
     transfer) never succeed while the ECU is locked, and nothing gets written
     to the staging slot.

The seeds are fixed so a failure is reproducible. The harness runs against the
host model; the same idea can be pointed at the real ECU over a transport.
"""
import random

from virtual_ecu import VirtualEcu, STAGING_SIZE

# The only negative response codes this server is allowed to emit. Anything else
# coming back is itself a finding.
VALID_NRC = {0x11, 0x12, 0x13, 0x24, 0x31, 0x33, 0x35, 0x36, 0x72}


def assert_wellformed(pdu, resp):
    assert isinstance(resp, (bytes, bytearray)) and len(resp) >= 1, \
        f"no/!bytes reply to {bytes(pdu).hex(' ')}"
    sid = pdu[0] if pdu else 0x00
    if resp[0] == 0x7F:
        assert len(resp) == 3, f"malformed negative response {resp.hex(' ')}"
        assert resp[1] == sid, f"NRC echoes wrong SID: {resp.hex(' ')}"
        assert resp[2] in VALID_NRC, f"unexpected NRC 0x{resp[2]:02X}"
    else:
        assert resp[0] == (sid + 0x40) & 0xFF, \
            f"reply 0x{resp[0]:02X} is neither positive for 0x{sid:02X} nor a negative response"


def test_fuzz_random_pdus_never_crash():
    rng = random.Random(0xC0DE)
    for _ in range(5000):
        pdu = bytes(rng.randint(0, 255) for _ in range(rng.randint(0, 16)))
        resp = VirtualEcu().request(pdu)          # must not raise
        assert_wellformed(pdu, resp)


def test_fuzz_reprogramming_denied_while_locked():
    rng = random.Random(0xBEEF)
    ecu = VirtualEcu()                            # never unlocked in this test
    privileged = (0x31, 0x34, 0x36)               # erase / download / transfer
    for _ in range(5000):
        sid = rng.choice(privileged)
        pdu = bytes([sid]) + bytes(rng.randint(0, 255) for _ in range(rng.randint(0, 14)))
        resp = ecu.request(pdu)
        assert resp[0] != (sid + 0x40), \
            f"privileged 0x{sid:02X} succeeded while locked: {resp.hex(' ')}"
    # Nothing may have reached flash: the staging slot is still fully erased.
    assert all(b == 0xFF for b in ecu.staging), "staging slot was modified while locked"


def test_fuzz_mutated_valid_sequence():
    # Take the real reprogramming steps and flip a byte here and there; the
    # server must still only ever answer with well-formed responses.
    rng = random.Random(0x1234)
    steps = [
        [0x10, 0x02],
        [0x27, 0x01],
        [0x27, 0x02, 0, 0, 0, 0],
        [0x31, 0x01, 0xFF, 0x00],
        [0x34, 0x00, 0x44, 0x08, 0x01, 0x50, 0x00, 0x00, 0x00, 0x00, 0x08],
        [0x36, 0x01, 0xDE, 0xAD, 0xBE, 0xEF],
        [0x37],
        [0x23, 0x44, 0x08, 0x01, 0x50, 0x00, 0x00, 0x00, 0x00, 0x08],
    ]
    for _ in range(3000):
        ecu = VirtualEcu()
        for step in steps:
            pdu = bytearray(step)
            if pdu and rng.random() < 0.3:
                pdu[rng.randrange(len(pdu))] = rng.randint(0, 255)
            resp = ecu.request(bytes(pdu))
            assert_wellformed(bytes(pdu), resp)
