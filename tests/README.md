# UDS conformance suite

An automated ISO 14229 conformance test for the bootloader's UDS server. It
drives every service the way a diagnostic tester would and checks each response
against the rule in `Core/Src/bl_uds.c` - both the services that must be
accepted and the malformed / out-of-turn requests that must be refused with a
specific negative response code.

By default it runs entirely on the PC, with no board and no CAN hardware, against
a host-side model of the server (`virtual_ecu.py`). The tests reach the ECU only
through the small `Uds` helper in `conftest.py`, so the same cases can also be
pointed at the real Nucleo (see *Running against real hardware*).

## Files

| File | Role |
|------|------|
| `virtual_ecu.py` | host model of the `bl_uds.c` UDS server (seed/key, session gate, staging window, CRC, NRCs) |
| `conftest.py` | the `Uds` client helper and the transport-selecting fixture |
| `test_uds_conformance.py` | the conformance cases (accepted services + negative-response matrix) |
| `test_uds_fuzz.py` | robustness / fuzz tests (malformed and out-of-turn requests) |
| `test_uds_hardware.py` | conformance for the production server, run against the real board |
| `uds_bus_sim.py` | the same exchange over a virtual CAN bus, with a frame trace |
| `prod_bridge.py` | `CMD_UDS`-over-bridge transport to the production server |
| `serial_bridge.py` | raw ISO-TP transport (standards server; needs raw-mode bridge firmware) |
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

## Running the firmware in the emulator

`renode/` boots the real Boot Manager and FBL in [Renode](https://renode.io) (1.17) and talks to
them with the real host tool, no board needed. Build with the published test keys first:

```
cmake -S . -B build-test -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DBL_TEST_KEYS=ON
cmake --build build-test
cmake -S . -B build-udsself -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DBL_TEST_KEYS=ON -DBL_UDS_SELFTEST=ON
cmake --build build-udsself
cd tests/renode
python -m pytest -m "not powercut"     # install, security, sessions, raw commands, both UDS servers (about 20 min)
python -m pytest -m powercut           # power cut at every kind of flash operation (about 30 min)
```

Set `RENODE_PATH` if `renode` is not on the PATH. Without Renode or the builds the tests skip.
Needs `pyserial` and `pynacl`.

## Running against real hardware

The firmware has two UDS servers, so there are two hardware paths.

### Production server over the CAN bridge — the working path

`test_uds_hardware.py` drives the shipping hand-rolled UDS server in
`bootloader.c` over the real CAN bus, through the Blue Pill bridge, using the
same `CMD_UDS` tunnel `host/bl_host.py` uses. Point `HW_PORT` at the bridge:

```bash
# Windows - through the Blue Pill bridge onto CAN
set HW_PORT=can:COM6
pytest tests/test_uds_hardware.py -v
```

`HW_PORT` also accepts a direct ST-Link COM (`set HW_PORT=COM3`, board held in
bootloader mode) to reach the FBL over UART. Without `HW_PORT` the module is
skipped, so CI and the model suite are unaffected. Every case is
**non-destructive** — it only stages a few bytes into the A/B staging slot and
never runs the install routine, so the live app is never touched.

**How it was verified:** all cases pass on hardware over the direct **ST-Link
UART** (`HW_PORT=COM3`, board in bootloader mode). The UDS handler is
transport-agnostic (`BL_ProcessFrame` dispatches the same `UDS_Handle` for every
link), so this validates the server across transports; the CAN path is separately
confirmed to carry it (`bl_host.py can:COMx` reads the version and DIDs over the
bus).

### Standards server — needs the raw-mode bridge

The model suite (`test_uds_conformance.py`) matches the standards `iso14229`
server (`bl_uds.c`), reached over a raw ISO-TP bridge mode. Setting
`UDS_TARGET=serial:COMx` points that suite at the board via `serial_bridge.py`,
but it needs bridge firmware that implements the raw passthrough (bus 3) and a
Nucleo built with `BL_UDS_SERVER_ON_BOOT = 1`. The current bridge firmware does
not answer that handshake, so this path is parked until that firmware lands.

## What it covers

- **Both servers alike** - `vectors/uds_common.txt` is replayed against the production command
  layer (`renode/test_uds_common.py`) and the iso14229 server (its on-chip self-test).
- **Sessions** - default / programming / extended accepted; an unknown
  sub-function is refused with `subFunctionNotSupported (0x12)`.
- **Unknown service** - refused with `serviceNotSupported (0x11)`.
- **Sessions** - the four sessions, which changes are allowed, the lock on a session change, S3, and
  functional addressing (`test_uds_sessions.py`); the C rules are compared with the model on every
  combination in `test_udspolicy.py`.
- **Security access** - the correct seed/key (first 4 bytes of AES-CMAC of the
  seed, see `host/seckey.py`) unlocks; a wrong key or a wrong-length key gives
  `invalidKey (0x35)`; repeated bad keys lock the level with
  `exceededNumberOfAttempts (0x36)`. The model uses the public demo key.
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
