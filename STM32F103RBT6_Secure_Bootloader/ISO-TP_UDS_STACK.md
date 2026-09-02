# Standards ISO-TP + UDS stack (isotp-c + iso14229)

A standards-compliant **ISO 15765-2 (ISO-TP)** transport and **ISO 14229-1 (UDS)** diagnostic
server, ported to the STM32F103 as an optional, self-contained subsystem alongside the
bootloader's existing hand-rolled command layer.

It was added in response to a suggestion on
[issue #1](https://github.com/AdhamEhab14/Three-Tier-STM32-Secure-Bootloader/issues/1) to port
two well-known open-source libraries instead of relying only on the minimal hand-rolled
ISO-TP/UDS-style code.

## Status: ported and verified, not yet live-integrated

- ✅ **Ported** to bare-metal STM32F103 (Cortex-M3, no RTOS).
- ✅ **Verified on host** — the real `iso14229.c` + `isotp.c` are compiled with the actual glue
  and driven through a full reprogramming sequence (see *Testing*).
- ✅ **Verified on hardware** — an on-board self-test runs the whole UDS sequence in software
  loopback on the Nucleo and reports pass/fail on LD2.
- ⏳ **Not compiled into the production FBL.** The full stack does not fit the FBL's 40 KB flash
  budget alongside the existing feature set (see *Flash / RAM budget*). Live integration is
  gated on a flash-map change and is intentionally left out; the production bootloader is
  byte-for-byte unaffected.

## Libraries

Vendored under [`Core/ThirdParty/`](Core/ThirdParty) with pinned upstream commits recorded in
[`Core/ThirdParty/UPSTREAM.md`](Core/ThirdParty/UPSTREAM.md):

| Library | Standard | Role |
| --- | --- | --- |
| [isotp-c](https://github.com/SimonCahill/isotp-c) | ISO 15765-2 | Segmentation / reassembly + flow control over CAN |
| [iso14229](https://github.com/driftregion/iso14229) | ISO 14229-1 | UDS server (services + state machine) |

> Under `UDS_SYS=UDS_SYS_CUSTOM`, iso14229 does **not** compile its own bundled copy of isotp-c,
> so there is no symbol clash with the standalone isotp-c we vendored. The two are bridged by our
> own glue instead.

## Architecture

```
UDS client (CAN 0x7E0 / 0x7E8)
        |
        v
iso14229 UDS server  ── bl_uds.c ──►  flash driver (FlashIf_*)
        |  (UDSTp_t bridge)
        v
isotp-c transport    ── bl_isotp.c ─►  CAN1 (bxCAN)
```

- [`Core/Src/bl_isotp.c`](Core/Src/bl_isotp.c) — isotp-c ↔ CAN1 glue: the three `isotp_user_*`
  callbacks, an RX pump that routes frames to links by CAN ID, and a software-loopback harness
  used by the self-tests.
- [`Core/Src/bl_uds.c`](Core/Src/bl_uds.c) — a `UDSTp_t` transport bridge wrapping one ISO-TP
  link, `UDSMillis()` from `HAL_GetTick()`, and the UDS server event handler.
- Addressing: requests on `0x7E0`, replies on `0x7E8` (UDS-style).

### UDS services implemented

| Service | SID | Behaviour |
| --- | --- | --- |
| DiagnosticSessionControl | `0x10` | default / programming / extended |
| SecurityAccess | `0x27` | seed/key unlock, level 1 (see security note) |
| RoutineControl | `0x31` | routine `0xFF00` erases the A/B staging slot |
| RequestDownload | `0x34` | validates the target lies in the staging slot |
| TransferData | `0x36` | streams blocks to the flash driver |
| RequestTransferExit | `0x37` | ends the download |
| ReadMemoryByAddress | `0x23` | reads the staging slot back (download verification) |
| ECUReset | `0x11` | accepts and resets the MCU |

The image is streamed into **Slot B** (staging). The existing command layer still owns the
Ed25519 verification and the A/B copy-into-Slot-A swap.

### Security note

The `0x27` seed/key relation is a lightweight session unlock, **not** the cryptographic root of
trust. Firmware images remain authenticated by their **Ed25519 signature at install time** — that
is unchanged by this stack.

## Build switches

All in [`Core/Inc/bl_config.h`](Core/Inc/bl_config.h) and `main.c`; every one defaults to off, so
the production build never links the stack:

| Macro | Default | Effect |
| --- | --- | --- |
| `BL_USE_ISO_STACK` | `0` | reserved for a future live integration |
| `BL_ISOTP_SELFTEST_ON_BOOT` | `0` | run the ISO-TP self-test at boot, halt on LD2 |
| `BL_UDS_SELFTEST_ON_BOOT` | `0` | run the UDS self-test at boot, halt on LD2 |

When a self-test macro is set, the normal bootloader body in `main()` is compiled out so the
throwaway image (HAL + the stack + the self-test) fits the 40 KB region.

## Testing

### On host

The real library + glue sources build and run on a PC with small HAL / flash stubs, driving the
whole sequence (session → seed/key → erase → RequestDownload → TransferData → exit) and checking
the payload lands at the staging address. This exercises the actual `bl_uds.c` / `bl_isotp.c` /
`iso14229.c` / `isotp.c` code, only stubbing `HAL_GetTick` and the flash driver.

### On the Nucleo (software loopback)

1. Set `BL_UDS_SELFTEST_ON_BOOT` to `1` in `main.c` (leave the others at `0`).
2. Build the **Release** configuration and flash.
3. Read **LD2**: solid ON = pass; otherwise it blinks a diagnostic code `1..9` (documented in
   [`Core/Inc/bl_uds.h`](Core/Inc/bl_uds.h)), pauses ~1.5 s, and repeats. A ~1 s pause before the
   result is normal — the test waits out the SecurityAccess brute-force boot delay.
4. Set the macro back to `0` for a normal build.

`BL_ISOTP_SELFTEST_ON_BOOT` does the same for the transport layer alone (codes in
[`Core/Inc/bl_isotp.h`](Core/Inc/bl_isotp.h)).

### On the real two-node CAN bus

The stack has also been driven end to end over a real CAN bus, node to node
(verified on hardware):

- **Nucleo** built with `BL_UDS_SERVER_ON_BOOT = 1` runs the live UDS server on CAN.
- **Blue Pill as UDS client** (`BP_UDS_CLIENT_ON_BOOT = 1` in the bridge project) drives the
  full sequence — session, seed/key, erase, RequestDownload, TransferData, RequestTransferExit,
  and a ReadMemoryByAddress **read-back that confirms the transferred bytes landed in flash** —
  and narrates the whole exchange over its USART1 (9600) plus a pass/fail LED.
- **PC as UDS client**: the bridge's raw ISO-TP passthrough mode (`SET_BRIDGE` bus 3) plus
  [`host/uds_client.py`](../host/uds_client.py) run the same sequence from the PC.

These are throwaway test builds; every switch defaults to `0`, so the production FBL and the
normal bridge are unchanged.

## Flash / RAM budget

Measured cost of the stack (`-Os`, cortex-m3):

| Object | Flash | RAM |
| --- | --: | --: |
| iso14229 | ~12.3 KB | — |
| isotp-c | ~2.3 KB | — |
| bl_isotp | ~0.8 KB | ~0.3 KB |
| bl_uds | ~1.1 KB | ~1.6 KB |
| **Total** | **~16.5 KB** | **~1.9 KB** |

The FBL region is **40 KB flash** and about **4 KB of low RAM** (below the RAM-resident SBL at
`0x20001000`). The production FBL already fills most of that (Ed25519/TweetNaCl, the command
layer, six transports), so it cannot also hold the full UDS server.

### To integrate it live

A live integration needs headroom the current map does not have. The realistic path is a
**flash-map change** — grow the FBL region (e.g. 40 → 64 KB), which shifts the application, Slot B,
and config regions and the Boot Manager's expectations. That is a deliberate, separate piece of
work and is **not** done here; this subsystem stays a verified, self-contained option until then.
