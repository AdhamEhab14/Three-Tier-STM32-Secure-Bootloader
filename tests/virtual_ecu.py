"""
virtual_ecu.py - a host-side model of the bootloader's UDS server.

It answers UDS requests the same way Core/Src/bl_uds.c does on the Nucleo:
the same seed/key relation, the same programming-session + security gate on the
reprogramming services, the same staging-slot address window, and the same
negative response codes. It keeps a small in-memory "staging slot" so that a
download followed by a read-back actually returns the bytes that were written.

The point is to have something the conformance suite can hammer with no board
and no CAN bus attached. The rules encoded here are the contract; the same test
cases can later be pointed at the real ECU to confirm it honours that contract.
"""

# ---- constants that mirror bl_uds.c / bootloader.h -------------------------

SECRET = bytes([0x19, 0x84, 0xC0, 0xDE])   # key = seed XOR SECRET
SEC_LEVEL = 0x01

STAGING_BASE = 0x08015000                  # SLOT_B_BASE
STAGING_SIZE = 0x7000                       # mirrors APP_MAX_SIZE (28 KB); keep in step with bootloader.h
MAX_BLOCK = 128                             # maxNumberOfBlockLength

# Sessions (DiagnosticSessionControl sub-functions)
SESSION_DEFAULT = 0x01
SESSION_PROGRAMMING = 0x02
SESSION_EXTENDED = 0x03

# Routines
RID_ERASE = 0xFF00
RID_CHECK = 0xFF01

# Negative response codes, named so the tests read like the spec.
NRC_SERVICE_NOT_SUPPORTED = 0x11
NRC_SUBFUNCTION_NOT_SUPPORTED = 0x12
NRC_INCORRECT_LENGTH = 0x13
NRC_REQUEST_OUT_OF_RANGE = 0x31
NRC_SECURITY_ACCESS_DENIED = 0x33
NRC_INVALID_KEY = 0x35
NRC_EXCEEDED_ATTEMPTS = 0x36

# After this many bad keys the server locks the level, matching the iso14229
# attempt limiter. Modelled here so the suite can exercise the 0x36 path.
MAX_KEY_ATTEMPTS = 3


def _u32(b):
    return int.from_bytes(b, "big")


class VirtualEcu:
    def __init__(self):
        self.session = SESSION_DEFAULT
        self.unlocked = False
        self.seed = None
        self.key_attempts = 0
        self.dl_addr = None
        # The staging slot starts erased (flash reads as 0xFF).
        self.staging = bytearray(b"\xFF" * STAGING_SIZE)

    # -- helpers --------------------------------------------------------------

    def _neg(self, sid, nrc):
        return bytes([0x7F, sid, nrc])

    def _in_staging(self, addr, size):
        return (
            size > 0
            and addr >= STAGING_BASE
            and (addr + size) <= (STAGING_BASE + STAGING_SIZE)
        )

    def _reprogramming_allowed(self):
        # Same two-part gate as bl_uds_reprogramming_allowed(): unlocked *and*
        # in the programming session.
        return self.unlocked and self.session == SESSION_PROGRAMMING

    @staticmethod
    def _crc32(data):
        # Reflected CRC-32, poly 0xEDB88320 - the routine CheckMemory returns.
        crc = 0xFFFFFFFF
        for byte in data:
            crc ^= byte
            for _ in range(8):
                crc = (crc >> 1) ^ 0xEDB88320 if (crc & 1) else (crc >> 1)
        return crc ^ 0xFFFFFFFF

    # -- entry point ----------------------------------------------------------

    def request(self, pdu):
        """Take a UDS request PDU (bytes), return the response PDU (bytes)."""
        if not pdu:
            return self._neg(0x00, NRC_SERVICE_NOT_SUPPORTED)

        sid = pdu[0]
        handler = self._handlers.get(sid)
        if handler is None:
            return self._neg(sid, NRC_SERVICE_NOT_SUPPORTED)
        return handler(self, pdu)

    # -- services -------------------------------------------------------------

    def _diagnostic_session_control(self, pdu):
        sub = pdu[1]
        if sub not in (SESSION_DEFAULT, SESSION_PROGRAMMING, SESSION_EXTENDED):
            return self._neg(0x10, NRC_SUBFUNCTION_NOT_SUPPORTED)
        self.session = sub
        # Positive reply carries the P2/P2* timing record; the firmware lets the
        # library fill it in. We echo four representative bytes.
        return bytes([0x50, sub, 0x00, 0x32, 0x01, 0xF4])

    def _security_access(self, pdu):
        sub = pdu[1]

        if sub == 0x01:                                  # requestSeed
            if self.unlocked:
                # Already unlocked: spec says hand back an all-zero seed.
                self.seed = bytes(4)
            else:
                # A deterministic non-zero seed keeps the test reproducible.
                self.seed = bytes([0xA5, 0x5A, 0xC3, 0x3C])
            return bytes([0x67, 0x01]) + self.seed

        if sub == 0x02:                                  # sendKey
            if self.key_attempts >= MAX_KEY_ATTEMPTS:
                return self._neg(0x27, NRC_EXCEEDED_ATTEMPTS)
            key = pdu[2:]
            expected = bytes(s ^ k for s, k in zip(self.seed, SECRET))
            if len(key) != 4 or key != expected:
                self.key_attempts += 1
                return self._neg(0x27, NRC_INVALID_KEY)
            self.unlocked = True
            self.key_attempts = 0
            return bytes([0x67, 0x02])

        return self._neg(0x27, NRC_SUBFUNCTION_NOT_SUPPORTED)

    def _routine_control(self, pdu):
        if not self._reprogramming_allowed():
            return self._neg(0x31, NRC_SECURITY_ACCESS_DENIED)

        sub = pdu[1]
        rid = (pdu[2] << 8) | pdu[3]

        if rid == RID_ERASE and sub == 0x01:
            self.staging = bytearray(b"\xFF" * STAGING_SIZE)
            return bytes([0x71, 0x01, 0xFF, 0x00])

        if rid == RID_CHECK and sub == 0x01:
            record = pdu[4:]
            if len(record) < 8:
                return self._neg(0x31, NRC_INCORRECT_LENGTH)
            addr, size = _u32(record[0:4]), _u32(record[4:8])
            if not self._in_staging(addr, size):
                return self._neg(0x31, NRC_REQUEST_OUT_OF_RANGE)
            off = addr - STAGING_BASE
            crc = self._crc32(self.staging[off:off + size])
            return bytes([0x71, 0x01, 0xFF, 0x01]) + crc.to_bytes(4, "big")

        return self._neg(0x31, NRC_REQUEST_OUT_OF_RANGE)

    def _request_download(self, pdu):
        if not self._reprogramming_allowed():
            return self._neg(0x34, NRC_SECURITY_ACCESS_DENIED)

        # [0x34][dfi][alfid][addr:4][size:4]
        addr, size = _u32(pdu[3:7]), _u32(pdu[7:11])
        if not self._in_staging(addr, size):
            return self._neg(0x34, NRC_REQUEST_OUT_OF_RANGE)
        self.dl_addr = addr
        # lengthFormatId 0x20 -> a 2-byte maxNumberOfBlockLength follows.
        return bytes([0x74, 0x20]) + MAX_BLOCK.to_bytes(2, "big")

    def _transfer_data(self, pdu):
        bsc = pdu[1]
        data = pdu[2:]
        off = self.dl_addr - STAGING_BASE
        self.staging[off:off + len(data)] = data
        self.dl_addr += len(data)
        return bytes([0x76, bsc])

    def _request_transfer_exit(self, pdu):
        return bytes([0x77])

    def _read_memory_by_address(self, pdu):
        # [0x23][alfid][addr:4][size:4]
        addr, size = _u32(pdu[2:6]), _u32(pdu[6:10])
        if size > MAX_BLOCK or not self._in_staging(addr, size):
            return self._neg(0x23, NRC_REQUEST_OUT_OF_RANGE)
        off = addr - STAGING_BASE
        return bytes([0x63]) + bytes(self.staging[off:off + size])

    def _ecu_reset(self, pdu):
        return bytes([0x51, pdu[1]])

    def _communication_control(self, pdu):
        return bytes([0x68, pdu[1]])

    def _control_dtc_setting(self, pdu):
        return bytes([0xC5, pdu[1]])

    _handlers = {
        0x10: _diagnostic_session_control,
        0x27: _security_access,
        0x31: _routine_control,
        0x34: _request_download,
        0x36: _transfer_data,
        0x37: _request_transfer_exit,
        0x23: _read_memory_by_address,
        0x11: _ecu_reset,
        0x28: _communication_control,
        0x85: _control_dtc_setting,
    }
