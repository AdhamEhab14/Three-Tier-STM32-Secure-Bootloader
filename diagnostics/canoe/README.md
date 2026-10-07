# CANoe / CAPL — UDS tester

`UdsTester.can` is a CAPL diagnostic tester for the bootloader's UDS server,
written to run as a node in **Vector CANoe or CANalyzer** on the diagnostic bus.

It drives the same reprogramming sequence as `host/uds_client.py` and the
`tests/` conformance suite — session, seed/key, erase, RequestDownload,
TransferData, RequestTransferExit, a ReadMemoryByAddress read-back, and a
RoutineControl CheckMemory CRC-32 — and reports each step to the Write window,
ending in `PASS` when the read-back and CRC match.

Requests are sent on `0x7E0`, replies read on `0x7E8`. Because most of the PDUs
span more than one CAN frame, the node carries a compact **ISO 15765-2 (ISO-TP)**
implementation of its own — single/first/consecutive frames plus the flow-control
handshake — mirroring the segmentation the firmware's `can_bl.c` does.

## Usage

Add the file as the CAPL program of a node in a CANoe/CANalyzer configuration on
the diagnostic channel, start the measurement, and press **`r`**. The Write
window shows the sequence advancing and the final result.

The key for a seed is the first 4 bytes of AES-CMAC (RFC 4493) of the seed, worked out by
`seckey.cin` (included by `UdsTester.can`). It holds the public demo key, which is what a
firmware built without its own key contains; put the product's key there for a real one. The
port is checked by compiling it as plain C against the RFC vectors (`tests/test_seckey.py`),
but it has not been run inside CANoe. The staging-slot base (`0x08015000`) and the payload
are constants at the top of `UdsTester.can`, matching `Core/Src/bl_uds.c`.
