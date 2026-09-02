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

The seed/key relation (`key = seed XOR 19 84 C0 DE`), the staging-slot base
(`0x08015000`) and the payload are constants at the top of the file, matching
`Core/Src/bl_uds.c`.
