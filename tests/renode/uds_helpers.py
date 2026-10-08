"""Small helpers for tests that talk UDS to the production bootloader."""
import bl_host

SESSION_DEFAULT = 0x01
SESSION_PROGRAMMING = 0x02
SESSION_EXTENDED = 0x03
SESSION_SAFETY = 0x04

NRC_SUBFUNCTION_NOT_SUPPORTED = 0x12
NRC_INVALID_LENGTH = 0x13
NRC_CONDITIONS_NOT_CORRECT = 0x22
NRC_OUT_OF_RANGE = 0x31
NRC_SECURITY_DENIED = 0x33
NRC_INVALID_KEY = 0x35
NRC_EXCEEDED = 0x36
NRC_TIME_DELAY = 0x37
NRC_NOT_IN_SESSION = 0x7F


def uds(ser, *pdu):
    """Send one UDS request through the bootloader's command layer; return the response PDU."""
    ok, p = bl_host.transact(ser, bl_host.CMD_UDS, bytes(pdu))
    assert ok, "no reply to the UDS request"
    return bytes(p)


def nrc(resp, sid):
    """The NRC of a negative response to `sid`."""
    assert resp[0] == 0x7F and resp[1] == sid, "expected a negative response to 0x%02X, got %s" % (sid, resp.hex())
    return resp[2]


def enter(ser, session):
    """Switch session and check it was accepted."""
    resp = uds(ser, 0x10, session)
    assert resp[:2] == bytes([0x50, session]), "session 0x%02X refused: %s" % (session, resp.hex())


def active_session(ser):
    """The session the board says it is in (DID 0xF186)."""
    resp = uds(ser, 0x22, 0xF1, 0x86)
    assert resp[:3] == bytes([0x62, 0xF1, 0x86]), resp.hex()
    return resp[3]


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
