# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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
