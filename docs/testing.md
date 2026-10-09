# Testing guide

How to test every part of this project yourself: what each kind of test checks, what you need
installed, and the exact commands. Most of it needs no board at all.

Commands are written for Windows PowerShell from the repository root unless a step says
otherwise. They work the same in a Linux or macOS shell, except where noted.

## 1. Which test checks what

| Part of the project | Checked by | Needs a board? | Section |
|---|---|---|---|
| The firmware builds, and fits its flash space | CMake build, size check | No | 3 |
| Code quality (warnings, bug patterns) | Strict build, clang-tidy, cppcheck | No | 4 |
| UDS rules, SecurityAccess algorithm, session table | Host tests (Python model + C compiled for the PC) | No | 5 |
| ODX description, CAN bus simulation | Host tools | No | 5 |
| The real Boot Manager and FBL: boot, signed install, rollback, tamper, encryption, security, sessions, raw-command fences, UDS flashing, both UDS servers | Emulator tests (Renode) | No | 6 |
| Recovery from a power cut at every kind of flash operation | Emulator power-cut tests | No | 6 |
| ESP32 Wi-Fi / BLE gateway compiles | PlatformIO build | No | 7 |
| The production UDS server on a real board | Hardware conformance suite | Yes | 8 |
| CAN, SPI, I2C, Wi-Fi, BLE, real flash, option-byte locks, a real plug pull | The bench test plan | Yes | 8 |

Every push and pull request runs sections 3 to 7 automatically in CI (section 9).

## 2. What to install

| Tool | Used for | Notes |
|---|---|---|
| Python 3.10 or newer | All test runners and host tools | Then `pip install -r tests/requirements.txt` |
| Arm GNU Toolchain (`arm-none-eabi-gcc`) | Building the firmware | From developer.arm.com; put its `bin` folder on the PATH |
| CMake 3.20+ and Ninja | Building the firmware | Both must be on the PATH |
| A host C compiler (`gcc`) | The C unit tests that run on the PC | On Windows, MinGW-w64; without it those tests are skipped |
| Renode 1.17 | Emulator tests (section 6) | The portable Windows zip from the Renode releases page; unzip anywhere |
| clang-tidy and cppcheck (optional) | Static analysis (section 4) | Easiest on Linux or WSL; CI runs them for you |
| PlatformIO (optional) | ESP32 gateway build (section 7) | `pip install platformio` |
| odxtools (optional) | ODX schema check | `pip install odxtools` |

Install the Python packages once:

```
pip install -r tests/requirements.txt
```

The tests find Renode by looking, in order, at the `RENODE_PATH` environment variable, a
`renode` command on the PATH, and `D:\Tools\renode\renode.exe`. If yours is elsewhere, point
`RENODE_PATH` at the executable for the current PowerShell window:

```
$env:RENODE_PATH = "C:\path\to\renode\renode.exe"
```

## 3. Build the firmware

A normal build needs your own keys (see the README, "Building and running"). For testing, the
project has published **test keys** in `tests/keys` that are only for emulators and CI:

```
cmake -S . -B build-test -G Ninja "-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake" -DBL_TEST_KEYS=ON
cmake --build build-test
```

The `-D` arguments are quoted because PowerShell otherwise splits them at the dot.

The build prints the size of each image. To check the headroom the way CI does (this script
needs a Bash shell, for example Git Bash):

```
bash scripts/size_check.sh build-test/fbl.elf 40960
```

It prints how many bytes are free, warns when fewer than 512 are left, and fails if the image
does not fit.

## 4. Static analysis

**Strict build:** extra warnings treated as errors, plus GCC's static analyzer, on the project's
own C files:

```
cmake -S . -B build-strict -G Ninja "-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake" -DBL_STRICT=ON -DBL_TEST_KEYS=ON
cmake --build build-strict
```

**clang-tidy and cppcheck** (Bash shell; needs both tools installed):

```
bash scripts/lint.sh               # both
bash scripts/lint.sh clang-tidy    # one of them
bash scripts/lint.sh cppcheck
```

It prints `lint: clean` when nothing is found. The checks it runs are listed in `.clang-tidy`.

## 5. Host tests (no board, no emulator, about 10 seconds)

These run on the PC only:

```
cd tests
python -m pytest -v --ignore=renode
```

| File | What it checks |
|---|---|
| `test_uds_conformance.py` | An ISO 14229 conformance suite against `virtual_ecu.py`, a Python model of the UDS server: accepted services and the negative-response matrix |
| `test_uds_fuzz.py` | Thousands of malformed and out-of-turn requests: no crash, always a well-formed answer, no privilege while locked |
| `test_uds_sessions.py` | The four sessions, allowed changes, re-lock on a change, S3 timeout, functional addressing |
| `test_udspolicy.py` | Compiles `bl_udspolicy.c` for the PC and compares the C rules table with the model on every combination |
| `test_seckey.py` | AES and AES-CMAC against the RFC 4493 and FIPS-197 vectors; the C, Python and CAPL key algorithms against each other |
| `test_uds_vectors_sync.py` | The C table of shared UDS requests matches `vectors/uds_common.txt` |
| `test_uds_hardware.py` | Skipped here; it needs a board (section 8) |

A pass/fail report you can open in a browser:

```
python -m pytest --ignore=renode --html=report.html --self-contained-html
```

**CAN bus simulation:** the full reprogramming sequence as real CAN frames on python-can's
virtual bus, segmented with ISO-TP, printing the frame trace (`--log` also writes captures for
BUSMASTER and SavvyCAN):

```
python uds_bus_sim.py
```

**ODX description** (from the repository root):

```
python diagnostics/odx/validate_refs.py diagnostics/odx/SecureBootloader.odx-d
python -m odxtools list diagnostics/odx/SecureBootloader.odx-d --services
```

## 6. Emulator tests (the real firmware, no board)

### What the emulator is, and where it lives

Renode is an open-source program that imitates a whole microcontroller board, so the exact
`.elf` files you would flash onto the Nucleo run on your PC instead, instruction by
instruction. Renode itself is installed separately (section 2). This repository holds what
makes Renode behave like this board, and the tests, all in `tests/renode/`:

| File | What it is |
|---|---|
| `platform/f103_bl.repl` | The board description: the STM32F103 plus the parts Renode's stock model lacks |
| `models/flash.py` | The flash controller: unlock, erase, program. It also counts every flash operation and can stop the processor at a chosen one, which is how the power-cut tests pull the plug |
| `models/rcc.py`, `models/can_stub.py` | The clock controller, and a CAN stub for start-up |
| `renode_session.py` | The remote control: starts Renode in the background, loads the firmware, moves emulated time forward in steps, types bytes into the emulated UART, power-cycles, saves and restores flash |
| `conftest.py` | pytest wiring: finds Renode and the builds, skips the tests cleanly if either is missing |
| `powercut.py`, `uds_helpers.py` | Helpers the tests share |
| `test_*.py` | The tests, one file per topic |

Renode runs hidden; no window opens. The tests use the real `host/bl_host.py` to talk to the
emulated board, so they exercise the same code you use with a real one.

### Run them

Build both test images first (from the repository root). The second one is the iso14229
server's on-chip self-test, used only by `test_uds_selftest.py`:

```
cmake -S . -B build-test -G Ninja "-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake" -DBL_TEST_KEYS=ON
cmake --build build-test
cmake -S . -B build-udsself -G Ninja "-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake" -DBL_TEST_KEYS=ON -DBL_UDS_SELFTEST=ON
cmake --build build-udsself
cd tests/renode
```

Then pick what to run:

| Command | Runs | Time (roughly) |
|---|---|---|
| `python -m pytest -v test_boot.py` | Boot and version check: the quick first test | 1 min |
| `python -m pytest -v "test_install.py::test_bad_signature_is_refused"` | One single test | 1 to 2 min |
| `python -m pytest -v test_security.py` | One topic | a few min |
| `python -m pytest -v -m "not powercut"` | Everything except power cuts | 20 to 30 min |
| `python -m pytest -v -m powercut` | Only the power-cut tests | 20 to 30 min |
| `python -m pytest -v` | Everything | 30 to 55 min |

Useful flags: `-v` lists each test as it passes or fails, `-x` stops at the first failure, `-rs`
shows why a test was skipped. Setting `$env:BL_THOROUGH = "1"` before the power-cut tests adds a
denser sweep of cut points.

| File | What it checks |
|---|---|
| `test_boot.py` | The Boot Manager starts the FBL; version and self-test answers |
| `test_install.py` | Signed install over UART; older version, bad signature and tampered payload refused |
| `test_encrypted_install.py` | Encrypted images decrypt and install; one encrypted under another key is refused before the app is touched |
| `test_raw_commands.py` | ERASE and WRITE only reach Slot B; the lock commands need an unlock |
| `test_security.py` | SecurityAccess: boot delay, correct key, single-use seeds, lockout after wrong keys |
| `test_sessions.py` | Session changes, per-service rules, S3 timeout, a session tied to the link that opened it |
| `test_uds_flash.py` | The whole UDS flash sequence with the real host tool |
| `test_uds_readmem.py` | ReadMemoryByAddress only reads what this session downloaded |
| `test_uds_common.py` | The production server answers the shared request list |
| `test_uds_selftest.py` | The iso14229 server passes its on-chip self-test, including the same shared list |
| `test_powercut_fbl.py` | Power cut at every stage of the bootloader's own update; the Boot Manager recovers |
| `test_powercut_app.py` | Power cut during an app install; the rollback floor survives |

### How the power-cut tests work

The flash model counts every erase and write. A test tells it "stop at operation N" or "stop
when the erase of page P starts", starts an update, and the processor freezes at that moment
as if the power went. The test saves the flash as it was left, clears RAM, boots again and
checks the board recovered. Cutting at every single operation would take hours, so the other
in-between states are generated by editing a copy of one real cut's flash image; those
generated states are trusted only because a test compares one of them with a real cut byte
for byte.

### Troubleshooting

- **Tests show "skipped".** Renode or a build was not found. Run with `-rs` to see which.
- **Changed the firmware, results did not change.** The tests run whatever is in `build-test`;
  rebuild it (`cmake --build build-test` from the repository root).
- **A test looks stuck.** It is usually just slow: the emulator runs the real firmware, including
  signature checks that take a second or two of emulated time.
- **Everything is suddenly slow.** A Renode left over from a cancelled run may be using the CPU.
  Close it with `taskkill /F /IM renode.exe` (Windows) or `pkill renode` (Linux).

## 7. ESP32 gateway build

```
pio run -d ESP32_OTA_Gateway
```

Without `ESP32_OTA_Gateway/src/gateway_secrets.h` it builds with the published default Wi-Fi
password and prints a warning; copy `gateway_secrets.example.h` to `gateway_secrets.h` to set
your own.

## 8. Tests on real hardware

**The production UDS server on a board.** Hold B1 and reset the Nucleo to enter the bootloader,
then point `HW_PORT` at its ST-Link COM port (or at the Blue Pill bridge, for example
`can:COM6`):

```
$env:HW_PORT = "COM3"
python -m pytest -v tests/test_uds_hardware.py
```

The cases only stage a few bytes into Slot B and never install, so the app on the board is not
touched. Without `HW_PORT` the file is skipped. Details are in `tests/README.md`.

**Everything only real silicon can show** (CAN, SPI, I2C, Wi-Fi and BLE transfers, real flash
timing, the WRP and RDP locks, recovery with the watchdog running, pulling the plug during an
update) is a step-by-step bench plan in [`docs/hardware-test.md`](hardware-test.md).

## 9. What CI runs

`.github/workflows/ci.yml` runs on every push and pull request:

| Job | Section |
|---|---|
| `firmware` | 3: build all four projects, flash headroom, the Debug build |
| `static-analysis` | 4: strict build, clang-tidy, cppcheck |
| `diagnostics` | 5: ODX checks, host tests, CAN bus simulation |
| `emulator` | 6: emulator tests except power cuts |
| `powercut` | 6: power-cut tests |
| `esp32` | 7: gateway build |

`main` only accepts changes through a pull request with all six green. Pushing a version tag
(`v*`) runs `.github/workflows/release.yml`, which reruns the host checks and publishes a
GitHub Release with the conformance report and the ODX file.

## 10. Adding a test

- A rule of the UDS server: add a case to the host model tests (section 5) and, when it touches
  the firmware's behavior, an emulator test in `tests/renode/`.
- A request both UDS servers must answer the same way: add a line to
  `tests/vectors/uds_common.txt`, run `python scripts/gen_uds_vectors.py`, and commit the
  regenerated header.
- A new emulator test: copy the pattern of `test_install.py` (the `session` and `ser` fixtures
  give you a booted board and a serial-like port to it).
- Check that a new test fails when the thing it guards is broken. A test that cannot fail
  proves nothing.
