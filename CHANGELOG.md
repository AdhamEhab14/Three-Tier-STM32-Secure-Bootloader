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

### Security
- **The image-encryption (ChaCha20) key was committed in `bootloader.c`**, so images encrypted
  with it were readable by anyone with the repository. It now comes from a git-ignored header
  (`sign_tool.py genenckey` writes `host/keys/bl_enckey.h`; CMake `-DBL_ENC_KEY_HEADER=...`).
  The old key is kept only as the public demo key. **Rotate it**: delete `host/keys/bl_enckey.bin`,
  run `genenckey`, rebuild, and re-sign any encrypted image. Signatures were never affected; the
  Ed25519 private key was never committed.
- The ESP32 gateway's Wi-Fi password can come from a git-ignored `gateway_secrets.h`.
- An encrypted image decrypted with the wrong key used to install as garbage over the working
  app, since the signature only covers the ciphertext. The image header (format 2) now carries
  the CRC of the plaintext in the field that was `reserved`, and the board checks it before
  erasing Slot A. **Encrypted images must be re-signed** with this `sign_tool.py`; plaintext
  images signed in format 1 still install.

### Changed
- SecurityAccess (UDS 0x27) answers a seed with the first 4 bytes of AES-CMAC (RFC 4493)
  instead of an XOR with a constant. Seeds are derived under the same key (they were a
  function of the tick), each works for one attempt, and a brute-force policy applies in
  both UDS servers: 1 s boot delay, 1 s after a wrong key, 10 s from the third in a row.
  Host tools, the Python model, the CAPL tester and the bridge self-test follow. A build
  without `BL_SEC_KEY_HEADER` uses the public demo key. Closes #5.

- UDS sessions follow a state machine: default, programming, extended and safety system;
  programming and safety are only reached through extended and only leave to default (NRC
  0x22 otherwise); a session change re-locks security and 5 s of silence (S3) returns to
  default. **Programming now needs `10 03` first**, so `bl_host.py udsflash`, `uds_client.py`,
  the CAPL tester and the bridge self-test enter extended first. Closes #4.
- Every UDS service has a row (sessions it works in, security level, physical/functional
  addressing) in `bl_udspolicy.c`, checked before the service runs, in both UDS servers.
  SecurityAccess is no longer available in the default session. The iso14229 server also
  accepts functional requests on 0x7DF. Closes #3.
- The framed ERASE and WRITE commands only accept addresses inside Slot B (page-aligned
  erases, even write addresses, and a write length that fits in the frame). They took any
  address before, so any transport could overwrite the app or the bootloader without a
  signature. Hosts only ever used Slot B, so `flash` and `updatefbl` are unaffected.

- **A CMake build now needs a SecurityAccess key**: `-DBL_SEC_KEY_HEADER=...`, or
  `-DBL_ALLOW_DEMO_KEY=ON` to get the public demo key on purpose (CI does). The CubeIDE build
  prints a compiler warning when it falls back to the demo key.
- The Debug build is `-Og` instead of `-O0` (CMake and the CubeIDE project): at `-O0` the FBL
  no longer fits its 40 KB region since the session rules went in. CI reports the headroom
  of the release and Debug images and warns when it gets low.
- The iso14229 server now gates every request where it arrives, so the answer does not depend
  on which check the library makes first (it used to answer `36` in the default session with
  0x24 instead of 0x7F), and it answers the active-session and version identifiers (0x22).
  The production handler answers `10` without a sub-function with 0x13, as the standard asks,
  and gained `23` (read back the staging slot, 64 bytes at most), `28` and `85`, which the
  service table lists and the iso14229 server already had. `bl_host.py udsflash` now sends
  `85 02` and `28 03 01` in the extended session before entering programming, as the standard
  sequence does.
  Found by replaying one shared request list against both servers (below).

### Tests
- `tests/vectors/uds_common.txt` is a list of requests with the start of the expected answer.
  The production command layer replays it in the emulator (`test_uds_common.py`) and the
  iso14229 server replays it in its on-chip self-test, so the two servers cannot drift apart
  unnoticed. `scripts/gen_uds_vectors.py` makes the C table from it; a test keeps them in step.
- The framed ERASE/WRITE fence (`test_raw_commands.py`).
- The session rules and service table on the real firmware (`test_sessions.py`), on the
  iso14229 server through its on-chip self-test run in the emulator, and in the Python model;
  the C table and the model are compared on every combination.
- AES-CMAC checked against the RFC 4493 and FIPS-197 vectors, and the C, Python and CAPL
  versions against each other on random inputs.
- SecurityAccess on the real firmware in the emulator: boot delay, key, single-use seeds,
  lockout.
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
