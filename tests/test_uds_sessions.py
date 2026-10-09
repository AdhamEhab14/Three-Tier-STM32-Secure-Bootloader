"""Diagnostic sessions and per-service rules, as the host model of the UDS server sees them.

The same behaviour is checked on the real firmware in tests/renode/test_sessions.py; the
model has to agree with it (tests/test_udspolicy.py compares the tables).

Run it (no board needed; from the repository root):

    cd tests
    python -m pytest -v test_uds_sessions.py
"""
import pytest

from test_uds_conformance import assert_nrc, assert_positive
from virtual_ecu import VirtualEcu, key_for_seed

DEFAULT, PROGRAMMING, EXTENDED, SAFETY = 0x01, 0x02, 0x03, 0x04

PATH_TO = {
    DEFAULT: [],
    EXTENDED: [EXTENDED],
    PROGRAMMING: [EXTENDED, PROGRAMMING],
    SAFETY: [EXTENDED, SAFETY],
}


def go(uds, session):
    for step in PATH_TO[session]:
        assert_positive(uds.send([0x10, step]), 0x10)


ALLOWED = [(DEFAULT, EXTENDED), (EXTENDED, PROGRAMMING), (EXTENDED, SAFETY),
           (PROGRAMMING, DEFAULT), (SAFETY, DEFAULT), (EXTENDED, DEFAULT)]
REFUSED = [(DEFAULT, PROGRAMMING), (DEFAULT, SAFETY), (PROGRAMMING, EXTENDED),
           (PROGRAMMING, SAFETY), (SAFETY, EXTENDED), (SAFETY, PROGRAMMING)]


@pytest.mark.parametrize("src,dst", ALLOWED)
def test_allowed_session_changes(uds, src, dst):
    go(uds, src)
    assert_positive(uds.send([0x10, dst]), 0x10)


@pytest.mark.parametrize("src,dst", REFUSED)
def test_refused_session_changes(uds, src, dst):
    go(uds, src)
    assert_nrc(uds.send([0x10, dst]), 0x10, 0x22)


def test_the_safety_session_exists(uds):
    go(uds, SAFETY)


def test_default_session_refuses_security_access_and_the_download_services(uds):
    assert_nrc(uds.send([0x27, 0x01]), 0x27, 0x7F)
    assert_nrc(uds.send([0x31, 0x01, 0xFF, 0x00]), 0x31, 0x7F)
    assert_nrc(uds.send([0x34, 0x00, 0x44, 0, 0, 0, 0, 0, 0, 0, 8]), 0x34, 0x7F)
    assert_nrc(uds.send([0x36, 0x01, 0xAA]), 0x36, 0x7F)
    assert uds.send([0x3E, 0x00]) == bytes([0x7E, 0x00])


def test_a_session_change_locks_security_again(uds):
    uds.unlock()
    assert uds.send([0x27, 0x01])[2:6] == bytes(4), "should be unlocked"
    assert_positive(uds.send([0x10, DEFAULT]), 0x10)
    go(uds, PROGRAMMING)
    assert uds.send([0x27, 0x01])[2:6] != bytes(4), "security stayed unlocked across a session change"


def model_of(uds):
    ecu = uds.transport
    if not isinstance(ecu, VirtualEcu):
        pytest.skip("needs the host model (the real board has no hook for S3 or functional addressing here)")
    return ecu


def test_s3_timeout_returns_to_default_and_locks(uds):
    model_of(uds)
    uds.unlock()
    uds.transport.s3_timeout()
    assert_nrc(uds.send([0x27, 0x01]), 0x27, 0x7F)
    go(uds, PROGRAMMING)
    assert_nrc(uds.send([0x34, 0x00, 0x44, 0x08, 0x01, 0x50, 0x00, 0, 0, 0, 8]), 0x34, 0x33)


def test_functional_requests_work_for_the_services_that_allow_them(uds):
    ecu = model_of(uds)
    assert ecu.request(bytes([0x3E, 0x00]), functional=True) == bytes([0x7E, 0x00])
    assert ecu.request(bytes([0x10, EXTENDED]), functional=True)[:2] == bytes([0x50, EXTENDED])


def test_physical_only_services_are_ignored_when_addressed_functionally(uds):
    ecu = model_of(uds)
    go(uds, EXTENDED)
    assert ecu.request(bytes([0x27, 0x01]), functional=True) is None, "a refused functional request must not be answered"
    assert ecu.request(bytes([0x31, 0x01, 0xFF, 0x00]), functional=True) is None


def test_the_unlock_key_works_in_the_extended_session_too(uds):
    go(uds, EXTENDED)
    seed = uds.send([0x27, 0x01])[2:6]
    assert_positive(uds.send([0x27, 0x02, *key_for_seed(seed)]), 0x27)
