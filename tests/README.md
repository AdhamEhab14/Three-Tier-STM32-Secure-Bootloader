# UDS conformance suite

An automated ISO 14229 conformance test for the bootloader's UDS server. It
drives every service the way a diagnostic tester would and checks each response
against the rule in `Core/Src/bl_uds.c` - both the services that must be
accepted and the malformed / out-of-turn requests that must be refused with a
specific negative response code.

It runs entirely on the PC, with no board and no CAN hardware, against a
host-side model of the server (`virtual_ecu.py`). The tests reach the ECU only
through the small `Uds` helper in `conftest.py`, so the same cases can later be
pointed at the real Nucleo by swapping in a bridge transport.

## Files

| File | Role |
|------|------|
| `virtual_ecu.py` | host model of the `bl_uds.c` UDS server (seed/key, session gate, staging window, CRC, NRCs) |
| `conftest.py` | the `Uds` client helper and the per-test fixture |
| `test_uds_conformance.py` | the 18 conformance cases |
| `uds_bus_sim.py` | the same exchange over a virtual CAN bus, with a frame trace |
| `requirements.txt` | `pytest`, `pytest-html`, `python-can` |

## Running it

```bash
cd tests
pip install -r requirements.txt
pytest --html=report.html --self-contained-html
```

`report.html` is the pass/fail report; open it in a browser. Add `-v` to list
each case by name in the terminal.

### Bus-level simulation

`uds_bus_sim.py` runs the full reprogramming sequence one layer lower - real CAN
frames on python-can's virtual bus, segmented with ISO-TP (single / first /
consecutive frames + flow control) between the tester and the VirtualEcu. No
hardware or CAN interface is needed. It prints each step, then the CAN frame
trace a bus monitor would have captured.

```bash
python uds_bus_sim.py
```

## What it covers

- **Sessions** - default / programming / extended accepted; an unknown
  sub-function is refused with `subFunctionNotSupported (0x12)`.
- **Unknown service** - refused with `serviceNotSupported (0x11)`.
- **Security access** - the correct seed/key (`key = seed XOR 19 84 C0 DE`)
  unlocks; a wrong key or a wrong-length key gives `invalidKey (0x35)`; repeated
  bad keys lock the level with `exceededNumberOfAttempts (0x36)`.
- **Security gate** - RoutineControl and RequestDownload are refused with
  `securityAccessDenied (0x33)` until the level is unlocked in a programming
  session.
- **Address range** - a download or read outside the staging slot, and an
  oversized download, give `requestOutOfRange (0x31)`; a short CheckMemory
  record gives `incorrectMessageLength (0x13)`.
- **Full sequence** - unlock -> erase -> RequestDownload -> TransferData ->
  RequestTransferExit -> ReadMemoryByAddress read-back -> CheckMemory, with the
  read-back matching the bytes sent and the server's CRC-32 matching one the
  tester computes independently.
