# Standards ISO-TP + UDS stack

A standards-compliant **ISO 15765-2 (ISO-TP)** transport and **ISO 14229-1 (UDS)** diagnostic
server ported to the STM32F103, provided as an optional, self-contained module alongside the
bootloader's own hand-rolled command layer. It is built on two established open-source libraries —
[isotp-c](https://github.com/SimonCahill/isotp-c) for the transport and
[iso14229](https://github.com/driftregion/iso14229) for the UDS server — bridged to the CAN1
peripheral and the flash driver by a thin glue layer.

Every build switch defaults off, so the production bootloader links none of it and is unchanged.
The module is kept out of the shipping image by design, to keep the FBL lean — see
[Footprint](#footprint).

## Libraries

Vendored under [`Core/ThirdParty/`](Core/ThirdParty) (source copied in rather than submoduled, so
STM32CubeIDE builds them straight from the tree). The exact upstream commits are pinned in
[`Core/ThirdParty/UPSTREAM.md`](Core/ThirdParty/UPSTREAM.md).

| Library | Standard | Role |
| --- | --- | --- |
| isotp-c | ISO 15765-2 | Segmentation, reassembly, and flow control over CAN |
| iso14229 | ISO 14229-1 | UDS server (services and state machine) |

Under `UDS_SYS=UDS_SYS_CUSTOM`, iso14229 does not compile its own bundled copy of isotp-c, so there
is no symbol clash with the standalone library; the two are bridged by this project's glue instead.

## Architecture

```
UDS client (CAN 0x7E0 / 0x7E8)
        |
        v
iso14229 UDS server  --  bl_uds.c  -->  flash driver (FlashIf_*)
        |  (UDSTp bridge)
        v
isotp-c transport    --  bl_isotp.c -->  CAN1 (bxCAN)
```

- **[`Core/Src/bl_isotp.c`](Core/Src/bl_isotp.c)** — isotp-c to CAN1 glue: the three user callbacks
  the library requires, plus an RX pump that routes each incoming frame to the correct link by CAN
  ID.
- **[`Core/Src/bl_uds.c`](Core/Src/bl_uds.c)** — a `UDSTp` transport bridge over one isotp-c link,
  `UDSMillis()` wired to `HAL_GetTick()`, and the UDS service handlers.
- **Addressing** — requests on `0x7E0`, replies on `0x7E8`.

### UDS services

| Service | SID | Behaviour |
| --- | --- | --- |
| DiagnosticSessionControl | `0x10` | default / programming / extended session |
| SecurityAccess | `0x27` | seed/key unlock (see [Security](#security)) |
| CommunicationControl | `0x28` | enable/disable normal communication during programming |
| ControlDTCSetting | `0x85` | suspend/resume DTC logging during programming |
| RoutineControl | `0x31` | routine `0xFF00` erases the staging slot; `0xFF01` returns a CRC-32 of a region (CheckMemory) |
| RequestDownload | `0x34` | validates the target lies inside the staging slot |
| TransferData | `0x36` | writes each block through the flash driver, block sequence counter checked |
| RequestTransferExit | `0x37` | ends the download |
| ReadMemoryByAddress | `0x23` | reads the staging slot back, for download verification |
| ECUReset | `0x11` | acknowledges, then resets the MCU |

New firmware is streamed into **Slot B**, the A/B staging slot. The Ed25519 signature check and the
copy-into-Slot-A swap remain the responsibility of the existing command layer.

### Security

The `0x27` seed/key is a session unlock, not the cryptographic root of trust. Firmware images remain
authenticated by their Ed25519 signature at install time, which this module does not change; the
seed/key only gates whether a client may start a download.

## Build switches

Defined in [`Core/Inc/bl_config.h`](Core/Inc/bl_config.h) and `main.c`, all default to `0`, so a
normal build never links the module:

| Macro | Project | Effect |
| --- | --- | --- |
| `BL_ISOTP_SELFTEST_ON_BOOT` | FBL | run the ISO-TP self-test at boot, result on LD2 |
| `BL_UDS_SELFTEST_ON_BOOT` | FBL | run the UDS self-test at boot, result on LD2 |
| `BL_UDS_SERVER_ON_BOOT` | FBL | run the live UDS server on CAN (two-board test) |
| `BP_UDS_CLIENT_ON_BOOT` | bridge | run the Blue Pill as a UDS client instead of the bridge |

When a switch is set, the normal bootloader body in `main()` is compiled out so the throwaway image
fits the 40 KB FBL region.

## Testing

Verified at three levels:

- **Host** — the real `iso14229.c`, `isotp.c`, and the glue compile and run on a PC with small HAL
  and flash stubs, driving the full sequence (session, seed/key, erase, RequestDownload,
  TransferData, exit, read-back, CheckMemory) and confirming the written bytes and their CRC.
- **On-chip software loopback** — the same sequence runs on the Nucleo with frames carried in a RAM
  ring, reporting pass/fail on LD2 (a blink code identifies the failing stage; see
  [`Core/Inc/bl_uds.h`](Core/Inc/bl_uds.h)).
- **Two-node CAN bus** — the Nucleo runs the live server; the sequence is driven either by the Blue
  Pill acting as a UDS client, or by the PC through the bridge's raw ISO-TP passthrough mode with
  [`host/uds_client.py`](../host/uds_client.py). Both include a ReadMemoryByAddress read-back that
  returns the freshly written bytes from flash.

## Footprint

Measured with `-Os` for Cortex-M3:

| Component | Flash | RAM |
| --- | --: | --: |
| iso14229 | ~12.3 KB | — |
| isotp-c | ~2.3 KB | — |
| bl_isotp | ~0.8 KB | ~0.3 KB |
| bl_uds | ~1.1 KB | ~1.6 KB |
| **Total** | **~16.5 KB** | **~1.9 KB** |

The FBL region provides 40 KB of flash and about 4 KB of low RAM (below the RAM-resident SBL), most
of which the production build already uses for the Ed25519 crypto, the command layer, and the
transports. The full UDS server therefore ships as this separate module rather than being linked
into the production image.
