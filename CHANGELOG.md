# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed (found by the power-cut tests)
- A power cut during the FBL self-update left the Boot Manager blinking forever; it now
  redoes the copy from Slot B.
- A damaged application image (for example after an interrupted install) halted the
  bootloader in the self-test; only a broken RAM or CRC engine does that now.
- A power cut while rewriting the app metadata reset the anti-rollback floor to zero, so
  an older image could be installed. The metadata is now kept in two pages and only the
  non-current one is rewritten. The record format changed (two new fields), so the
  first install after upgrading re-seeds the floor.

### Tests
- Emulator tests that cut power at every kind of flash operation during an FBL
  self-update and an app install (`tests/renode`, marker `powercut`).

### Build
- CMake build for all four firmware projects with plain `arm-none-eabi-gcc`
  (no CubeIDE needed), and a CI job that compiles them and keeps the `.bin` files.

## [1.0.0] - 2026-09-02

First tagged release of the three-tier secure bootloader, hardware-verified on
the Nucleo-F103RB.

### Firmware
- Three-tier architecture: immutable, WRP-lockable Boot Manager -> updatable
  Flash Bootloader -> application, with a verified boot chain.
- Ed25519-signed firmware, ChaCha20 encryption, and version anti-rollback.
- A/B staging, watchdog boot-trial recovery, and a power-on self-test (BIST).
- Self-update of the bootloader over any transport, staged and verified.
- One command protocol over six transports: UART, CAN (ISO-TP), SPI, I2C,
  Wi-Fi, and BLE.
- A working ISO 14229 UDS reprogramming sequence, plus a self-contained
  standards-library port (isotp-c + iso14229) kept out of the shipping FBL.

### Diagnostics & validation
- ISO 22901 (ODX) description of the UDS services, validated with odxtools.
- ISO 14229 conformance suite and UDS fuzz/robustness tests (pytest) over a
  host model of the server, with an HTML report.
- UDS sequence simulated over a virtual CAN bus, with a frame trace.
- CAPL tester for CANoe / CANalyzer.
- CI (GitHub Actions) running the ODX checks and the test suites on every push,
  and a release workflow publishing the report and ODX on each version tag.
