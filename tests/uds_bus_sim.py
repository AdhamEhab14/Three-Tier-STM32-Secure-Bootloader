"""
uds_bus_sim.py - the UDS reprogramming sequence running over a virtual CAN bus.

Where test_uds_conformance.py checks the server at the UDS message level, this
runs the same exchange one layer lower: real CAN frames on python-can's virtual
bus, segmented with ISO-TP (single / first / consecutive frames + flow control),
between a tester and the VirtualEcu. No hardware and no CAN interface needed -
the virtual bus is entirely in software.

It prints the sequence step by step, then the CAN frame trace a bus monitor
would have captured (arbitration id + data), which is the screenshot-worthy bit.

    python uds_bus_sim.py
"""
import sys
import threading
import time

import can

from virtual_ecu import VirtualEcu, SECRET, STAGING_BASE

CHANNEL = "udsbus"
REQ_ID = 0x7E0     # tester -> ECU
RESP_ID = 0x7E8    # ECU -> tester

PAYLOAD = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44])


def make_bus():
    return can.Bus(interface="virtual", channel=CHANNEL, receive_own_messages=False)


class IsoTp:
    """Minimal ISO 15765-2 over one CAN channel: send/recv whole UDS payloads."""

    def __init__(self, bus, tx_id, rx_id):
        self.bus = bus
        self.tx_id = tx_id
        self.rx_id = rx_id

    def _tx(self, data):
        frame = bytes(data) + bytes(8 - len(data))          # pad to 8 bytes
        self.bus.send(can.Message(arbitration_id=self.tx_id, data=frame[:8],
                                  is_extended_id=False))

    def _rx(self, timeout=2.0):
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            msg = self.bus.recv(timeout=remaining)
            if msg is not None and msg.arbitration_id == self.rx_id:
                return msg.data

    def send(self, payload):
        payload = bytes(payload)
        n = len(payload)
        if n <= 7:
            self._tx(bytes([n]) + payload)
            return
        # First frame carries the 12-bit length + the first 6 bytes.
        self._tx(bytes([0x10 | ((n >> 8) & 0x0F), n & 0xFF]) + payload[:6])
        fc = self._rx()
        if fc is None or (fc[0] & 0xF0) != 0x30:
            raise RuntimeError("no flow control after first frame")
        idx, sn = 6, 1
        while idx < n:
            chunk = payload[idx:idx + 7]
            self._tx(bytes([0x20 | (sn & 0x0F)]) + chunk)
            idx += len(chunk)
            sn = (sn + 1) & 0x0F

    def recv(self, timeout=2.0):
        first = self._rx(timeout)
        if first is None:
            return None
        pci = first[0] & 0xF0
        if pci == 0x00:                                     # single frame
            length = first[0] & 0x0F
            return bytes(first[1:1 + length])
        if pci == 0x10:                                     # first frame
            total = ((first[0] & 0x0F) << 8) | first[1]
            buf = bytearray(first[2:8])
            self._tx(bytes([0x30, 0x00, 0x00]))             # flow control: clear to send
            while len(buf) < total:
                cf = self._rx(timeout)
                if cf is None:
                    return None
                take = min(7, total - len(buf))
                buf += cf[1:1 + take]
            return bytes(buf[:total])
        return None


def ecu_worker(stop):
    """The bootloader side: reassemble a request, answer it, repeat."""
    bus = make_bus()
    tp = IsoTp(bus, RESP_ID, REQ_ID)
    ecu = VirtualEcu()
    while not stop.is_set():
        req = tp.recv(timeout=0.3)
        if req is not None:
            tp.send(ecu.request(req))
    bus.shutdown()


def monitor_worker(stop, trace):
    """A passive listener that records every frame on the bus for the trace."""
    bus = make_bus()
    while not stop.is_set():
        msg = bus.recv(timeout=0.3)
        if msg is not None:
            trace.append((time.time(), msg.arbitration_id, bytes(msg.data)))
    bus.shutdown()


# The whole exchange finishes in a few milliseconds, so the captured timestamps
# would all collapse to the same value. For a readable trace we space the frames
# a fixed 10 ms apart in capture order - the ordering and the data are real, only
# the inter-frame gap is synthetic.
FRAME_STEP_S = 0.010


def write_busmaster_log(path, trace):
    """Write the frames in BUSMASTER's .log format so they open in its trace view."""
    with open(path, "w") as f:
        f.write("***BUSMASTER Ver 3.2.2***\n")
        f.write("***PROTOCOL CAN***\n")
        f.write("***[START LOGGING SESSION]***\n")
        f.write("***HEX***\n")
        f.write("***SYSTEM MODE***\n")
        f.write("***<Time><Tx/Rx><Channel><CAN ID><Type><DLC><DataBytes>***\n")
        for i, (_ts, arb_id, data) in enumerate(trace):
            rel = i * FRAME_STEP_S
            hh, mm, ss, cs = int(rel // 3600) % 24, int(rel // 60) % 60, int(rel) % 60, int((rel * 100) % 100)
            direction = "Tx" if arb_id == REQ_ID else "Rx"
            payload = " ".join(f"{b:02X}" for b in data)
            f.write(f" {hh:02d}:{mm:02d}:{ss:02d}:{cs:02d} {direction} 1 0x{arb_id:X} s 8 {payload}\n")
        f.write("***[STOP LOGGING SESSION]***\n")


def write_savvycan_csv(path, trace):
    """Write a SavvyCAN-style CSV (its 'Generic' import), a reliable offline viewer."""
    with open(path, "w") as f:
        f.write("Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8\n")
        for i, (_ts, arb_id, data) in enumerate(trace):
            cells = [f"{b:02X}" for b in data] + [""] * (8 - len(data))
            direction = "Tx" if arb_id == REQ_ID else "Rx"
            f.write(f"{int(i * FRAME_STEP_S * 1e6)},{arb_id:08X},false,{direction},0,{len(data)},"
                    + ",".join(cells) + "\n")


def main():
    stop = threading.Event()
    trace = []
    threading.Thread(target=ecu_worker, args=(stop,), daemon=True).start()
    threading.Thread(target=monitor_worker, args=(stop, trace), daemon=True).start()
    time.sleep(0.2)   # let the workers open their bus handles

    bus = make_bus()
    tp = IsoTp(bus, REQ_ID, RESP_ID)
    ok = True

    def step(label, request, expect_prefix):
        nonlocal ok
        tp.send(request)
        resp = tp.recv()
        good = resp is not None and resp[:len(expect_prefix)] == bytes(expect_prefix)
        print(f"  [{'ok ' if good else 'FAIL'}] {label:<22} <- {resp.hex(' ') if resp else '(no reply)'}")
        ok = ok and good
        return resp

    print("=== UDS reprogramming over a virtual CAN bus (0x7E0/0x7E8) ===")

    step("SessionControl", [0x10, 0x02], [0x50, 0x02])

    seed = step("requestSeed", [0x27, 0x01], [0x67, 0x01])[2:6]
    key = bytes(s ^ k for s, k in zip(seed, SECRET))
    step("sendKey", [0x27, 0x02, *key], [0x67, 0x02])

    step("erase staging", [0x31, 0x01, 0xFF, 0x00], [0x71, 0x01, 0xFF, 0x00])

    rd = [0x34, 0x00, 0x44] + list(STAGING_BASE.to_bytes(4, "big")) + list((8).to_bytes(4, "big"))
    step("RequestDownload", rd, [0x74])
    step("TransferData", [0x36, 0x01, *PAYLOAD], [0x76, 0x01])
    step("RequestTransferExit", [0x37], [0x77])

    rmba = [0x23, 0x44] + list(STAGING_BASE.to_bytes(4, "big")) + list((8).to_bytes(4, "big"))
    readback = step("ReadMemoryByAddr", rmba, [0x63])
    if readback[1:9] != PAYLOAD:
        ok = False
        print("  [FAIL] read-back did not match the transferred bytes")

    cm = [0x31, 0x01, 0xFF, 0x01] + list(STAGING_BASE.to_bytes(4, "big")) + list((8).to_bytes(4, "big"))
    check = step("CheckMemory CRC", cm, [0x71, 0x01, 0xFF, 0x01])
    if int.from_bytes(check[4:8], "big") != VirtualEcu._crc32(PAYLOAD):
        ok = False
        print("  [FAIL] CheckMemory CRC did not match")

    time.sleep(0.1)
    stop.set()
    bus.shutdown()

    print("\n=== CAN frame trace ===")
    print("   ID    data")
    for _ts, arb_id, data in trace:
        print(f"  {arb_id:03X}   {data.hex(' ')}")

    if "--log" in sys.argv:
        write_busmaster_log("uds_trace.log", trace)
        write_savvycan_csv("uds_trace.csv", trace)
        print("\nWrote uds_trace.log (BUSMASTER) and uds_trace.csv (SavvyCAN).")

    print("\n==== RESULT:", "PASS ====" if ok else "FAIL ====")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
