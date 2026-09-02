#!/usr/bin/env python3
"""
uds_client.py - drive the Nucleo's iso14229 UDS server over real CAN, from the
PC, through the Blue Pill bridge's raw ISO-TP passthrough mode (BUS_CAN_RAW).

Setup:
  - Nucleo flashed with BL_UDS_SERVER_ON_BOOT = 1 (live UDS server on CAN).
  - Blue Pill running the normal bridge firmware (BP_UDS_CLIENT_ON_BOOT = 0).
  - PC on the bridge's USB-serial link.

Usage:
  python uds_client.py COM6          (Windows)
  python uds_client.py /dev/ttyUSB0  (Linux)

The bridge relays each request straight onto CAN (0x7E0) and returns the raw
ISO-TP reply, so this script speaks UDS directly - no CRC, no framing wrapper.
"""
import sys
import time
import serial   # pip install pyserial

SECRET = bytes([0x19, 0x84, 0xC0, 0xDE])   # must match bl_uds.c
SLOT_B = 0x08015000                        # A/B staging slot base
PAYLOAD = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44])


def fail(msg):
    print(f"\n==== RESULT: FAIL - {msg} ====")
    sys.exit(1)


def open_raw(port):
    """Open the port and put the bridge into raw ISO-TP passthrough mode.

    The bridge acks SET_BRIDGE at 115200, then drops the link to 9600 (this
    clone's USART1 can be marginal at 115200), so we follow it down to 9600.
    """
    s = serial.Serial(port, 115200, timeout=2)
    time.sleep(0.3)
    ack = b""
    for _ in range(6):
        s.baudrate = 115200
        s.reset_input_buffer()
        s.write(bytes([0x02, 0xE0, 0x03]))     # SET_BRIDGE -> BUS_CAN_RAW
        ack = s.read(3)
        if len(ack) == 3 and ack[0] == 0xCD and ack[2] == 0x03:
            s.baudrate = 9600                  # bridge switched to 9600 after acking
            time.sleep(0.1)
            s.reset_input_buffer()
            s.timeout = 6
            return s
        time.sleep(0.2)
    fail("bridge did not enter raw mode (ack=%s). Is the Blue Pill running the "
         "NORMAL bridge (BP_UDS_CLIENT_ON_BOOT = 0), not the client firmware?"
         % (ack.hex(' ') if ack else 'none'))


def xfer(s, label, pdu):
    """Send one UDS request as [LEN][PDU]; read the reply as [RLEN][reply]."""
    print(f"\n{label}")
    print("   -> req", pdu.hex(" "))
    s.write(bytes([len(pdu)]) + pdu)
    hdr = s.read(1)
    if not hdr:
        fail("no reply (timeout) - is the Nucleo server running and the bus wired?")
    rlen = hdr[0]
    rsp = s.read(rlen) if rlen else b""
    print("   <- rsp", rsp.hex(" ") if rsp else "(failure)")
    return rsp


def need(cond, msg):
    if not cond:
        fail(msg)


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: python uds_client.py <serial port>")

    s = open_raw(sys.argv[1])
    print("=== PC UDS client -> Blue Pill (raw CAN) -> Nucleo iso14229 server ===")

    # 1) DiagnosticSessionControl -> programming session
    r = xfer(s, "[1] DiagnosticSessionControl (programming)", bytes([0x10, 0x02]))
    need(r[:2] == bytes([0x50, 0x02]), "session control not accepted")

    print("    waiting out the ~1 s security boot delay...")
    time.sleep(1.3)

    # 2) SecurityAccess requestSeed
    r = xfer(s, "[2] SecurityAccess requestSeed", bytes([0x27, 0x01]))
    need(len(r) >= 6 and r[:2] == bytes([0x67, 0x01]), "seed not granted")
    seed = r[2:6]
    key = bytes(a ^ b for a, b in zip(seed, SECRET))
    print("    seed from server:", seed.hex(" "), " computed key:", key.hex(" "))

    # 3) SecurityAccess sendKey
    r = xfer(s, "[3] SecurityAccess sendKey", bytes([0x27, 0x02]) + key)
    need(r[:2] == bytes([0x67, 0x02]), "key rejected")
    print("    -> security unlocked")

    # 4) RoutineControl start -> erase staging slot
    r = xfer(s, "[4] RoutineControl: erase staging slot (0xFF00)", bytes([0x31, 0x01, 0xFF, 0x00]))
    need(r[:2] == bytes([0x71, 0x01]), "erase routine rejected")

    # 5) RequestDownload: 8 bytes at SLOT_B
    dl = bytes([0x34, 0x00, 0x44]) + SLOT_B.to_bytes(4, "big") + (8).to_bytes(4, "big")
    r = xfer(s, f"[5] RequestDownload: 8 bytes @ 0x{SLOT_B:08X}", dl)
    need(r[:1] == bytes([0x74]), "request download rejected")

    # 6) TransferData block #1
    r = xfer(s, "[6] TransferData block #1", bytes([0x36, 0x01]) + PAYLOAD)
    need(r[:2] == bytes([0x76, 0x01]), "transfer data rejected")

    # 7) RequestTransferExit
    r = xfer(s, "[7] RequestTransferExit", bytes([0x37]))
    need(r[:1] == bytes([0x77]), "transfer exit rejected")

    # 8) ReadMemoryByAddress -> read the bytes back and verify
    rd = bytes([0x23, 0x44]) + SLOT_B.to_bytes(4, "big") + (8).to_bytes(4, "big")
    r = xfer(s, "[8] ReadMemoryByAddress: read 8 bytes back", rd)
    need(r[:1] == bytes([0x63]) and r[1:9] == PAYLOAD, "read-back does not match")
    print("    -> read-back matches the transferred bytes:", r[1:9].hex(" "))

    print("\n==== RESULT: PASS (full UDS reprogramming + verified read-back over real CAN) ====")
    s.close()


if __name__ == "__main__":
    main()
