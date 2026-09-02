"""
serial_bridge.py - a transport that lets the conformance suite talk to the real
Nucleo instead of the in-process model.

It drives the Blue Pill bridge in its raw ISO-TP passthrough mode, exactly like
host/uds_client.py: each UDS request goes out as [LEN][PDU], and the bridge
relays it onto CAN (0x7E0) and returns the ISO-TP reply as [RLEN][reply]. So a
`request(pdu) -> response` here reaches the board's iso14229 server over real CAN.

Selected by the UDS_TARGET environment variable (see conftest.py). Nothing here
is imported unless a hardware run is requested, so CI never needs pyserial.

Board setup for a hardware run:
  - Nucleo flashed with BL_UDS_SERVER_ON_BOOT = 1 (live UDS server on CAN)
  - Blue Pill running the normal bridge firmware (BP_UDS_CLIENT_ON_BOOT = 0)
  - PC on the bridge's USB-serial port
"""
import time


class SerialBridge:
    def __init__(self, port):
        import serial   # pyserial - imported lazily so CI stays serial-free
        self._serial_mod = serial
        self.s = self._open_raw(port)

    def _open_raw(self, port):
        """Open the port and put the bridge into raw ISO-TP passthrough mode.

        The bridge acks SET_BRIDGE at 115200 then drops to 9600 (the clone's
        USART1 is marginal at 115200), so we follow it down - same handshake as
        uds_client.py.
        """
        s = self._serial_mod.Serial(port, 115200, timeout=2)
        time.sleep(0.3)
        for _ in range(6):
            s.baudrate = 115200
            s.reset_input_buffer()
            s.write(bytes([0x02, 0xE0, 0x03]))          # SET_BRIDGE -> BUS_CAN_RAW
            ack = s.read(3)
            if len(ack) == 3 and ack[0] == 0xCD and ack[2] == 0x03:
                s.baudrate = 9600
                time.sleep(0.1)
                s.reset_input_buffer()
                s.timeout = 6
                return s
            time.sleep(0.2)
        raise RuntimeError(
            "bridge did not enter raw ISO-TP mode - is the Blue Pill running the "
            "normal bridge firmware (BP_UDS_CLIENT_ON_BOOT = 0)?"
        )

    def request(self, pdu):
        pdu = bytes(pdu)
        self.s.write(bytes([len(pdu)]) + pdu)
        hdr = self.s.read(1)
        if not hdr:
            raise RuntimeError("no reply from ECU (timeout) - is the server running and the bus wired?")
        rlen = hdr[0]
        return self.s.read(rlen) if rlen else b""

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass
