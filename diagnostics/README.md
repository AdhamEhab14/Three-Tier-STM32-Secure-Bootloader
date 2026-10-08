# Diagnostics — ODX description

A Vector-ecosystem diagnostic artifact for the bootloader's **standards UDS server**
(`STM32F103RBT6_Secure_Bootloader/Core/Src/bl_uds.c`, built on `iso14229` + `isotp-c`).

It is **off-target tooling** — it adds nothing to the FBL's flash budget. It describes the UDS
server that already exists; it does not change the firmware.

## Contents

| Path | What it is |
|------|------------|
| `odx/SecureBootloader.odx-d` | ISO 22901-1 (ODX) description of the UDS server |
| `odx/validate_refs.py` | dependency-free structural check (unique IDs, resolvable ID-REFs) |

## `odx/SecureBootloader.odx-d`

A hand-authored **ODX-D** (ODX 2.2.0) diagnostic description. ODX is the open ISO 22901
format that Vector's diagnostic tools (ODX Studio, Indigo, CANoe's diagnostic layer) load, so
this is the license-free way to produce a genuine Vector-ecosystem artifact.

It describes exactly the services in `bl_uds.c` — request/positive-response/negative-response
byte layouts, the seed/key relation, and the NRC value table — for physical addressing on
**request `0x7E0` / response `0x7E8`**, ISO-TP over CAN at 250 kbit/s.

### Service map (ODX ⇄ `bl_uds.c`)

| Service | SID | ODX service | `bl_uds.c` event |
|---|---|---|---|
| DiagnosticSessionControl | `0x10` | `DiagnosticSessionControl` | `UDS_EVT_DiagSessCtrl` |
| SecurityAccess requestSeed | `0x27 01` | `SecurityAccess_RequestSeed` | `UDS_EVT_SecAccessRequestSeed` |
| SecurityAccess sendKey | `0x27 02` | `SecurityAccess_SendKey` | `UDS_EVT_SecAccessValidateKey` |
| RoutineControl — erase staging | `0x31 01 FF00` | `RoutineControl_EraseStaging` | `UDS_EVT_RoutineCtrl` |
| RoutineControl — CheckMemory | `0x31 01 FF01` | `RoutineControl_CheckMemory` | `UDS_EVT_RoutineCtrl` |
| RequestDownload | `0x34` | `RequestDownload` | `UDS_EVT_RequestDownload` |
| TransferData | `0x36` | `TransferData` | `UDS_EVT_TransferData` |
| RequestTransferExit | `0x37` | `RequestTransferExit` | `UDS_EVT_RequestTransferExit` |
| ReadMemoryByAddress | `0x23` | `ReadMemoryByAddress` | `UDS_EVT_ReadMemByAddr` |
| ECUReset | `0x11` | `ECUReset` | `UDS_EVT_EcuReset` |
| CommunicationControl | `0x28` | `CommunicationControl` | `UDS_EVT_CommCtrl` |
| ControlDTCSetting | `0x85` | `ControlDTCSetting` | `UDS_EVT_ControlDTCSetting` |
| ReadDataByIdentifier, active session | `0x22 F186` | `ReadDataByIdentifier_ActiveSession` | `UDS_EVT_ReadDataByIdent` |
| ReadDataByIdentifier, bootloader version | `0x22 F195` | `ReadDataByIdentifier_BootloaderVersion` | `UDS_EVT_ReadDataByIdent` |
| TesterPresent | `0x3E` | `TesterPresent` | handled by the library |

Not described here (not part of the standards stack): `0x22 ReadDataByIdentifier` lives only
in the hand-rolled command layer, not in `bl_uds.c`.

### Key facts encoded

- **Seed/key:** 4-byte seed; the key is the first 4 bytes of AES-CMAC (RFC 4493) of the seed under a
  128-bit key, security level `0x01`. No `0x27` for 1 s after power-up (`0x37`); after a wrong key 1 s
  before the next try, from the 3rd wrong key in a row 10 s (`0x36` while waiting).
- **Download window:** address must lie inside the A/B **staging slot** (`SLOT_B_BASE 0x08015000`,
  `APP_MAX_SIZE`), else `requestOutOfRange (0x31)`. `maxNumberOfBlockLength = 128`.
- **CheckMemory CRC:** reflected CRC-32, poly `0xEDB88320`, init `0xFFFFFFFF`, final XOR.
- **Sessions:** `01` default, `02` programming, `03` extended, `04` safety system. Default goes to extended
  only; extended to any; programming and safety only back to default (`0x22` otherwise). A session change
  re-locks security; 5 s without a request (S3) or a reset means default.
- **Per-service rules:** each service has the sessions it works in, the security level it needs and the
  addressing it accepts (`0x7F` wrong session, `0x33` not unlocked, `0x11` physical-only service addressed
  functionally). The table is in `bl_udspolicy.c`.
- **NRC table:** `0x11 0x12 0x13 0x22 0x31 0x33 0x35 0x36 0x37 0x72 0x7F` — the exact set the server and
  the iso14229 library emit.

### Validating / opening it

```bash
pip install odxtools
python -m odxtools list diagnostics/odx/SecureBootloader.odx-d --services
```

This is a hand-authored description targeting the ODX-D 2.2 schema; `odxtools` (or an ODX-aware
tool) is the reference validator.
