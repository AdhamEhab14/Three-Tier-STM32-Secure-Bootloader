"""
prod_bridge.py - transport for the hardware conformance run.

It reaches the production UDS server (the hand-rolled one in bootloader.c) the
same way host/bl_host.py does: each UDS PDU is carried as the payload of the
CMD_UDS (0x20) framed command, over whatever transport the port string selects -
including `can:COMx`, which routes through the Blue Pill bridge onto the CAN bus.

A dropped reply just gets re-sent: every request here is a read or an idempotent
step, and a framing-level miss means the board never acted on it, so a re-send is
safe. That smooths over the clone bridge's occasional lost frame.
"""
import os
import sys


class ProdBridge:
    def __init__(self, port, tries=4):
        host_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "host"))
        if host_dir not in sys.path:
            sys.path.insert(0, host_dir)
        import bl_host
        self._bl = bl_host
        self._tries = tries
        # Short timeout so a dropped frame is retried quickly rather than stalling
        # for the default 20 s; every production UDS op here answers fast.
        self.ser = bl_host.open_transport(port, timeout=4)

    def request(self, pdu):
        """Send a UDS PDU, return the response PDU (positive or [0x7F][sid][nrc])."""
        pdu = bytes(pdu)
        last = b""
        for _ in range(self._tries):
            # Drop any stale/late bytes so a previous reply can't desync this read
            # - the clone bridge occasionally corrupts or delays a frame at 115200.
            if hasattr(self.ser, "reset_input_buffer"):
                self.ser.reset_input_buffer()
            ok, payload = self._bl.transact(self.ser, self._bl.CMD_UDS, pdu)
            if ok:
                return payload
            last = payload
        return last

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass
