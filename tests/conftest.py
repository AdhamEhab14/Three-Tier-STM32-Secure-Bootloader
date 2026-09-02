"""
Shared fixtures for the UDS conformance suite.

The tests talk to the ECU only through the small `Uds` helper, so the same cases
run against two backends without changing a single test body:

  - by default, the in-process VirtualEcu (host model) - this is what CI uses;
  - when UDS_TARGET is set, the real Nucleo over the Blue Pill bridge, e.g.
        UDS_TARGET=serial:COM6   (Windows)   or   UDS_TARGET=/dev/ttyUSB0
    Point it at a board flashed with BL_UDS_SERVER_ON_BOOT = 1. Note the shared
    board state between tests: the full reprogramming sequence test is the
    self-contained one to run against hardware (see tests/README.md).
"""
import os
import time

import pytest

from virtual_ecu import VirtualEcu, SECRET


class Uds:
    """Thin UDS client: send a request PDU, get the response PDU back."""

    def __init__(self, transport):
        self._transport = transport

    def send(self, pdu):
        return self._transport.request(bytes(pdu))

    def enter_programming(self):
        return self.send([0x10, 0x02])

    def unlock(self):
        """Run the session + seed/key dance and return the sendKey response.

        On real hardware SecurityAccess is blocked for about a second after boot
        (RequiredTimeDelayNotExpired, 0x37); we ride that out so a hardware run
        doesn't fail purely on boot timing. The model never returns 0x37, so this
        loop runs exactly once there.
        """
        self.send([0x10, 0x02])
        for _ in range(25):
            seed_resp = self.send([0x27, 0x01])
            if seed_resp[:2] == bytes([0x67, 0x01]):
                break
            if len(seed_resp) >= 3 and seed_resp[0] == 0x7F and seed_resp[2] == 0x37:
                time.sleep(0.2)
                continue
            break
        seed = seed_resp[2:6]
        key = bytes(s ^ k for s, k in zip(seed, SECRET))
        return self.send([0x27, 0x02, *key])


@pytest.fixture
def uds():
    """A UDS client for each test - the host model, or the real board if UDS_TARGET is set."""
    target = os.environ.get("UDS_TARGET")
    if not target:
        yield Uds(VirtualEcu())
        return

    from serial_bridge import SerialBridge
    port = target.split(":", 1)[1] if target.startswith("serial:") else target
    bridge = SerialBridge(port)
    try:
        yield Uds(bridge)
    finally:
        bridge.close()
