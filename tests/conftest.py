"""
Shared fixtures for the UDS conformance suite.

The tests talk to the ECU only through the small `Uds` helper below, so the same
cases can later run against the real Nucleo: swap the in-process VirtualEcu for a
transport that forwards the PDU over the serial/CAN bridge, and nothing in the
test bodies changes.
"""
import pytest

from virtual_ecu import VirtualEcu, SECRET


class Uds:
    """Thin UDS client: send a request PDU, get the response PDU back."""

    def __init__(self, ecu):
        self._ecu = ecu

    def send(self, pdu):
        return self._ecu.request(bytes(pdu))

    def enter_programming(self):
        return self.send([0x10, 0x02])

    def unlock(self):
        """Run the session + seed/key dance and return the sendKey response."""
        self.send([0x10, 0x02])
        seed = self.send([0x27, 0x01])[2:6]
        key = bytes(s ^ k for s, k in zip(seed, SECRET))
        return self.send([0x27, 0x02, *key])


@pytest.fixture
def uds():
    """A fresh, locked ECU in the default session for each test."""
    return Uds(VirtualEcu())
