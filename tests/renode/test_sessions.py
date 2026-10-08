"""Diagnostic sessions (UDS 0x10) and the per-service rules, on the real firmware.

Default can only go to extended; programming and safety are reached through extended and
leave to default; a reset or S3 timeout means default. Each service also has the sessions it
works in and the security it needs (see bl_udspolicy.c, which tests/native checks on its own).
"""
import pytest

import bl_host
import seckey
from renode_session import RenodeSerial
from uds_helpers import (NRC_CONDITIONS_NOT_CORRECT, NRC_NOT_IN_SESSION, NRC_SECURITY_DENIED,
                         NRC_SUBFUNCTION_NOT_SUPPORTED, SESSION_DEFAULT, SESSION_EXTENDED,
                         SESSION_PROGRAMMING, SESSION_SAFETY, active_session, enter, nrc, uds,
                         wait_for_fbl)


@pytest.fixture()
def board(session):
    """A freshly powered-up board in its power-on (default) session."""
    session.power_cycle(settle=0.0)
    ser = RenodeSerial(session, timeout=5)
    wait_for_fbl(session, ser)
    return session, ser


def test_the_board_starts_in_the_default_session(board):
    _, ser = board
    assert active_session(ser) == SESSION_DEFAULT


ALLOWED = [
    (SESSION_DEFAULT, SESSION_EXTENDED),
    (SESSION_EXTENDED, SESSION_PROGRAMMING),
    (SESSION_EXTENDED, SESSION_SAFETY),
    (SESSION_PROGRAMMING, SESSION_DEFAULT),
    (SESSION_SAFETY, SESSION_DEFAULT),
]

REFUSED = [
    (SESSION_DEFAULT, SESSION_PROGRAMMING),
    (SESSION_DEFAULT, SESSION_SAFETY),
    (SESSION_PROGRAMMING, SESSION_EXTENDED),
    (SESSION_PROGRAMMING, SESSION_SAFETY),
    (SESSION_SAFETY, SESSION_EXTENDED),
    (SESSION_SAFETY, SESSION_PROGRAMMING),
]

PATH_TO = {
    SESSION_DEFAULT: [],
    SESSION_EXTENDED: [SESSION_EXTENDED],
    SESSION_PROGRAMMING: [SESSION_EXTENDED, SESSION_PROGRAMMING],
    SESSION_SAFETY: [SESSION_EXTENDED, SESSION_SAFETY],
}


def go(ser, session):
    for step in PATH_TO[session]:
        enter(ser, step)


@pytest.mark.parametrize("src,dst", ALLOWED, ids=lambda s: "%02X" % s)
def test_allowed_session_changes(board, src, dst):
    _, ser = board
    go(ser, src)
    enter(ser, dst)
    assert active_session(ser) == dst


@pytest.mark.parametrize("src,dst", REFUSED, ids=lambda s: "%02X" % s)
def test_refused_session_changes_leave_the_session_alone(board, src, dst):
    _, ser = board
    go(ser, src)
    assert nrc(uds(ser, 0x10, dst), 0x10) == NRC_CONDITIONS_NOT_CORRECT
    assert active_session(ser) == src


@pytest.mark.parametrize("sub", [0x00, 0x05, 0x7F])
def test_an_unknown_session_is_not_supported(board, sub):
    _, ser = board
    assert nrc(uds(ser, 0x10, sub), 0x10) == NRC_SUBFUNCTION_NOT_SUPPORTED


def test_default_session_refuses_what_needs_a_bigger_one(board):
    _, ser = board
    assert nrc(uds(ser, 0x27, 0x01), 0x27) == NRC_NOT_IN_SESSION
    assert nrc(uds(ser, 0x34, 0x00, 0x44, 0x08, 0x01, 0x50, 0x00, 0, 0, 0, 0x10), 0x34) == NRC_NOT_IN_SESSION
    assert nrc(uds(ser, 0x36, 0x01, 0xAA), 0x36) == NRC_NOT_IN_SESSION
    assert nrc(uds(ser, 0x31, 0x01, 0xFF, 0x01), 0x31) == NRC_NOT_IN_SESSION
    assert uds(ser, 0x3E, 0x00) == bytes([0x7E, 0x00]), "TesterPresent must work in every session"


def test_download_needs_the_programming_session_and_security(board):
    _, ser = board
    enter(ser, SESSION_EXTENDED)
    request = (0x34, 0x00, 0x44, 0x08, 0x01, 0x50, 0x00, 0, 0, 0, 0x10)
    assert nrc(uds(ser, *request), 0x34) == NRC_NOT_IN_SESSION       # extended is not enough
    enter(ser, SESSION_PROGRAMMING)
    assert nrc(uds(ser, *request), 0x34) == NRC_SECURITY_DENIED      # right session, still locked


def test_a_session_change_locks_security_again(board):
    session, ser = board
    enter(ser, SESSION_EXTENDED)
    enter(ser, SESSION_PROGRAMMING)
    session.idle(1.3)                                  # out of the boot delay
    assert bl_host.uds_unlock(ser) is True
    assert uds(ser, 0x27, 0x01)[2:6] == bytes(4), "should be unlocked"
    enter(ser, SESSION_DEFAULT)
    enter(ser, SESSION_EXTENDED)
    seed = uds(ser, 0x27, 0x01)
    assert seed[2:6] != bytes(4), "security stayed unlocked across a session change"


def test_silence_for_5_seconds_ends_a_non_default_session(board):
    session, ser = board
    enter(ser, SESSION_EXTENDED)
    session.idle(4.5)
    assert active_session(ser) == SESSION_EXTENDED, "dropped before the 5 s timeout"
    session.idle(5.5)                                  # more than 5 s since that last request
    assert active_session(ser) == SESSION_DEFAULT, "still in the extended session after S3"
    assert nrc(uds(ser, 0x27, 0x01), 0x27) == NRC_NOT_IN_SESSION


def test_tester_present_keeps_the_session_alive(board):
    session, ser = board
    enter(ser, SESSION_EXTENDED)
    for _ in range(4):
        session.idle(3.0)                              # 12 s in all, never 5 s of silence
        assert uds(ser, 0x3E, 0x00) == bytes([0x7E, 0x00])
    assert active_session(ser) == SESSION_EXTENDED


def test_the_timeout_also_drops_security(board):
    session, ser = board
    enter(ser, SESSION_EXTENDED)
    enter(ser, SESSION_PROGRAMMING)
    session.idle(1.3)
    assert bl_host.uds_unlock(ser) is True
    session.idle(6.0)
    assert active_session(ser) == SESSION_DEFAULT
    enter(ser, SESSION_EXTENDED)
    enter(ser, SESSION_PROGRAMMING)
    request = (0x34, 0x00, 0x44, 0x08, 0x01, 0x50, 0x00, 0, 0, 0, 0x10)
    assert nrc(uds(ser, *request), 0x34) == NRC_SECURITY_DENIED
