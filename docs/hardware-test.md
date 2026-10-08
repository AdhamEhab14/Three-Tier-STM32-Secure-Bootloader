# Hardware test plan

The emulator suite (`tests/renode`) covers the core of the bootloader without a board: boot,
signed install, rollback and tamper rejection, SecurityAccess, sessions, power cuts, the
UDS flash sequence. What it cannot reach is anything that needs a real peripheral or real
timing. This page is the bench check for that remainder, in the order that finds problems
fastest. Tick each step; stop at the first failure and fix it before going on.

Parts: Nucleo-F103RB with its ST-Link, USB cable. Later steps also use the Blue Pill bridge,
two MCP2551 transceivers (or a second node), and the ESP32 gateway.

## 0. Build

```
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \n      -DBL_SEC_KEY_HEADER=host/keys/bl_seckey.h -DBL_ENC_KEY_HEADER=host/keys/bl_enckey.h
cmake --build build
```

If you build in CubeIDE instead, refresh the projects first (new source files), and use the
Debug configuration only for stepping through code: it is `-Og` now.

Run `python host/sign_tool.py genkey`, `genenckey` and `genseckey` first, and paste the public
key into `bootloader.c` as the README says. CMake refuses to build the bootloader without the
two key headers; `-DBL_ALLOW_DEMO_KEY=ON` gets the public demo keys, which is fine for a
bench but not for a board that leaves the desk.

## 1. Flash and boot (UART only, ST-Link only)

1. If the Boot Manager was write-protected by an older build, clear WRP0-3 in
   STM32CubeProgrammer first (Option Bytes, untick, Apply). Then flash `boot_manager.bin`.
2. Flash `fbl.bin` at `0x08004000`, then `application.bin` at `0x0800E000`.
3. Press reset with nothing held. Expect: LD2 blinks for about 0.6 s (Boot Manager), then
   the application runs.
4. Hold **B1**, press reset. Expect: LD2 flickers, then stays quiet while the FBL waits.
5. `python host/bl_host.py COMx bist`. Expect: RAM PASS, CRC PASS, supply 3.3 V in range.

Note: the app metadata format changed (see CHANGELOG). A board that had an app installed by
an older bootloader needs it installed once more (step 2 below) before the app boots.

## 2. Signed install over UART

1. `python host/sign_tool.py sign app.bin 1.0.0` then `python host/bl_host.py COMx flash app.bin`.
   Expect: staged, verified, promoted, app starts.
2. Same with version `0.9.0`. Expect: rejected as a rollback.
3. Flip one byte in the signed file and flash it. Expect: rejected, the old app still boots.
4. Sign with `enc` and flash. Expect: installs and runs.

## 3. Raw commands are fenced

With the board in the FBL, send ERASE or WRITE to an address outside Slot B (a short script
using `bl_host.transact` with `CMD_ERASE` at `0x0800E000`). Expect a reply with result `0`
and the app untouched. Slot B itself still accepts both, which step 2 already proved.

## 4. UDS over the ST-Link port

1. `python host/bl_host.py COMx udsinfo`. Expect: identifiers print.
2. `python host/bl_host.py COMx udsflash app.bin`. Expect: extended, programming, unlock,
   download, install, reset, new app running.
3. Right after power-up, ask for a seed twice within a second. Expect: the first gets
   NRC `0x37` (the script waits it out).
4. Send a wrong key three times in a row. Expect: NRC `0x35` for each, then `0x36` for ten
   seconds, with the 1 s wait between attempts in the first two.
5. Request a seed in the default session. Expect: `7F 27 7F`. Request `10 02` straight from
   default. Expect: `7F 10 22`.
6. Leave the board idle in the programming session for more than 5 s, then ask for a seed.
   Expect: `7F 27 7F`, because S3 returned it to default and locked it.

## 5. Other transports (Blue Pill bridge, ESP32)

Repeat step 2 (a plain `flash`) once per link and note the time.

| Link | Command | Notes |
|---|---|---|
| CAN | `bl_host.py can:COMx flash app.bin` | 250 kbit/s, 120 ohm termination at both ends |
| SPI | `bl_host.py spi:COMx flash app.bin` | DATA_READY wired to PB1 |
| I2C | `bl_host.py i2c:COMx flash app.bin` | address `0x42`, pull-ups present |
| Wi-Fi | `bl_host.py tcp:192.168.4.1:3333 flash app.bin` | join the ESP32 access point first |
| BLE | `bl_host.py ble:STM32-OTA-BLE flash app.bin` | needs `bleak` |

## 6. iso14229 server on CAN

This is the second UDS implementation (`bl_uds.c`), built only when asked for.

1. Build the FBL with `-DBL_UDS_SELFTEST=ON` and flash it. Expect LD2 to light after boot
   (the on-chip self-test passed). The emulator already runs this one, so a failure here
   points at the CAN peripheral.
2. With the Blue Pill built as `-DBP_UDS_CLIENT=ON` on the same bus, power both. Expect the
   bridge to complete extended, programming, unlock and a small download.
3. With any CAN tool, send `3E 00` on `0x7DF` (functional). Expect `7E 00` on `0x7E8`.
   Send `27 01` on `0x7DF`. Expect nothing at all.

## 7. Power loss on the bench

The emulator proves the recovery logic at every flash operation. This checks the real thing
once. Do each twice.

1. Start `flash app.bin` (plain install). Pull USB when the ERASE of Slot B is running, which
   is the first second after the command. Reconnect holding **B1**. Expect the FBL answers
   `GET_VER` and a fresh `flash` works.
2. Same during the promote into Slot A (the last second before the app starts). Expect the
   FBL is reachable and the app is either the old one or absent, never half-written.
3. Start `updatefbl new_fbl.bin` and pull USB while the FBL region is being rewritten. Expect
   the Boot Manager restores the FBL from Slot B and the FBL comes up on its own.

## 8. Self-update and lock

1. `updatefbl` with a newer signed FBL on a healthy board. Expect: reboot into the new FBL,
   `GET_VER` shows the new version.
2. `lockbm`. Expect: reset, the board still boots. Check WRP in CubeProgrammer shows the
   Boot Manager pages protected. Clear it again with the steps in the README when finished.

## What to write down

For each step: pass or fail, the transfer time for step 5, and anything that behaved
differently from the line above. A failure in 1 to 4 is a firmware bug to report. A failure
in 5 or 6 alone is most likely wiring, termination or a bus peripheral issue.
