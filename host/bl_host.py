#!/usr/bin/env python3
"""
Author: Adham Ehab   Date: 18/08/2026

Host tool for the STM32F103RB bootloader.

Same commands, any transport - only the port argument changes:

  COM3                    straight into the FBL over the ST-Link COM port
  can:COM6 / spi:COM6 / i2c:COM6   through the Blue Pill bridge onto that bus
  tcp:192.168.4.1:3333    over the ESP32 Wi-Fi gateway
  ble:STM32-OTA-BLE       over the ESP32 BLE gateway

  bl_host.py COM3                     read the bootloader version
  bl_host.py COM3 flash app.bin       erase, program, verify, launch
  bl_host.py COM3 bist                read the power-on self-test result
  bl_host.py COM3 udsinfo             read a few UDS data identifiers
  bl_host.py COM3 udsflash app.bin    flash via the full UDS sequence
  bl_host.py COM3 updatefbl fbl.bin   reprogram the bootloader itself
  bl_host.py COM3 lockbm              write-protect the Boot Manager

Requires: pip install pyserial   (plus 'bleak' only for a ble: transport)
"""
import sys
import struct
import socket
import serial

# ---- protocol constants ----
CMD_GET_VER = 0x10
CMD_GO      = 0x14
CMD_ERASE   = 0x15
CMD_WRITE   = 0x16
CMD_VERIFY  = 0x17
CMD_UPDATE_FBL   = 0x1B
CMD_LOCK_BM      = 0x1C
CMD_BIST         = 0x1D
CMD_UDS          = 0x20   # wraps a UDS (ISO 14229) request as its payload
ACK, NACK   = 0xCD, 0xAB

APP_BASE = 0x0800E000        # where the application lives (Slot A)
CONFIG_ADDR = 0x0801FC00     # metadata page (must match the firmware)
SLOT_B      = 0x08015000     # staging slot (must match the firmware)
PAGE     = 1024              # F103 flash page size
CHUNK    = 64                # bytes per WRITE frame (smaller = shorter I2C transactions)


class TcpSerial:
    """Serial-like wrapper over a TCP socket, for the ESP32 WiFi gateway.

    Exposes just the read/write/close that this tool uses. read(n) accumulates
    until n bytes arrive or the timeout elapses, then returns what it has - the
    same "up to n within timeout" behaviour pyserial gives us."""
    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)

    def write(self, data):
        self.sock.sendall(data)

    def read(self, n):
        buf = bytearray()
        while len(buf) < n:
            try:
                chunk = self.sock.recv(n - len(buf))
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
        return bytes(buf)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class BleSerial:
    """Serial-like wrapper over the BLE Nordic UART Service (the ESP32 gateway).

    Bytes written go to the NUS RX characteristic; the STM32's replies arrive as
    notifications on NUS TX and are buffered here. bleak is async, so we run its
    event loop in a background thread and hand this class a plain read/write API.
    Needs: pip install bleak"""
    NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
    NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

    def __init__(self, name, timeout):
        import asyncio, threading
        self.timeout = timeout
        self._buf = bytearray()
        self._lock = threading.Lock()
        self._loop = asyncio.new_event_loop()
        threading.Thread(target=self._loop.run_forever, daemon=True).start()
        self._client = None
        self._submit(self._connect(name)).result(timeout=40)

    def _submit(self, coro):
        import asyncio
        return asyncio.run_coroutine_threadsafe(coro, self._loop)

    async def _connect(self, name):
        from bleak import BleakScanner, BleakClient
        print(f"Scanning for BLE device '{name}' ...")
        dev = await BleakScanner.find_device_by_name(name, timeout=15)
        if dev is None:
            raise RuntimeError(f"BLE device '{name}' not found (is the ESP32 powered and advertising?)")
        self._client = BleakClient(dev)
        await self._client.connect()

        def on_notify(_, data):
            with self._lock:
                self._buf.extend(data)

        await self._client.start_notify(self.NUS_TX, on_notify)
        print("BLE connected.")

    def write(self, data):
        for i in range(0, len(data), 180):          # keep each write within one PDU
            self._submit(self._client.write_gatt_char(
                self.NUS_RX, bytes(data[i:i + 180]), response=True)).result(timeout=self.timeout)

    def read(self, n):
        import time
        end = time.time() + self.timeout
        while True:
            with self._lock:
                if len(self._buf) >= n:
                    out = bytes(self._buf[:n]); del self._buf[:n]; return out
            if time.time() >= end:
                with self._lock:
                    out = bytes(self._buf[:n]); del self._buf[:len(out)]; return out
            time.sleep(0.005)

    def close(self):
        try:
            self._submit(self._client.disconnect()).result(timeout=5)
        except Exception:
            pass
        self._loop.call_soon_threadsafe(self._loop.stop)


CMD_SET_BRIDGE = 0xE0   # tells the Blue Pill bridge which downstream bus to use
BUS_CAN, BUS_SPI, BUS_I2C = 0, 1, 2


def _select_bridge_bus(ser, bus):
    """Send the Blue Pill a one-time SET_BRIDGE so it routes over CAN/SPI/I2C."""
    ser.write(build_frame(CMD_SET_BRIDGE, bytes([bus])))
    resp = ser.read(2)
    if len(resp) == 2 and resp[0] == ACK:
        ser.read(resp[1])   # consume the echoed bus id
        return True
    return False


def open_transport(port, timeout=20):
    """Transport selector:
         COMx            - serial (direct to the FBL, or a bridge left on its default bus)
         can:COMx        - Blue Pill bridge, route over CAN
         spi:COMx        - Blue Pill bridge, route over SPI
         i2c:COMx        - Blue Pill bridge, route over I2C (when available)
         tcp:<host>:<port> - ESP32 WiFi gateway
         ble:<name>      - ESP32 BLE gateway
    """
    if port.startswith("tcp:"):
        _, host, tport = port.split(":")
        return TcpSerial(host, int(tport), timeout)
    if port.startswith("ble:"):
        return BleSerial(port[4:], timeout)
    for prefix, bus in (("can:", BUS_CAN), ("spi:", BUS_SPI), ("i2c:", BUS_I2C)):
        if port.startswith(prefix):
            ser = serial.Serial(port[len(prefix):], 115200, timeout=timeout)
            if not _select_bridge_bus(ser, bus):
                print(f"Warning: bridge did not acknowledge {prefix[:-1].upper()} mode")
            return ser
    return serial.Serial(port, 115200, timeout=timeout)


# ---- STM32 hardware CRC (CRC-32/MPEG-2), one zero-extended word per byte ----
def stm32_crc(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(32):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if (crc & 0x80000000) \
                  else (crc << 1) & 0xFFFFFFFF
    return crc


def build_frame(cmd: int, data: bytes = b"") -> bytes:
    length = 1 + len(data) + 4
    head   = bytes([length, cmd]) + data
    return head + struct.pack("<I", stm32_crc(head))


def transact(ser, cmd, data=b""):
    """Send one frame, return (ok, payload). ok=False means NACK/no reply."""
    ser.write(build_frame(cmd, data))
    resp = ser.read(2)
    if len(resp) == 2 and resp[0] == ACK:
        return True, ser.read(resp[1])
    return False, resp


def transact_retry(ser, cmd, data=b"", tries=5):
    """Like transact, but re-send on a NACK/no-reply. SPI and I2C have no flow
    control, so a busy bus can occasionally corrupt a command frame - its CRC then
    fails on the board and nothing is written, which makes a plain re-send safe.
    A proper reply (even one whose result byte is 0, e.g. a legit VERIFY rejection)
    is returned immediately, not retried."""
    ok, resp = False, b""
    for _ in range(tries):
        ok, resp = transact(ser, cmd, data)
        if ok:
            return ok, resp
    return ok, resp


def ok_reply(ok, payload):
    return ok and len(payload) >= 1 and payload[0] == 1


def bist(ser):
    ok, p = transact(ser, CMD_BIST)
    if ok and len(p) == 5:
        vdd = p[3] | (p[4] << 8)
        print("Power-on self-test:")
        print("  RAM march     :", "PASS" if p[0] else "FAIL")
        print("  CRC + app CRC :", "PASS" if p[1] else "FAIL")
        print(f"  Supply (VDD)  : {vdd} mV", "(OK)" if p[2] else "(out of 2.7-3.6 V)")
    else:
        print("BIST - no/bad reply:", ok, p.hex(" "))


# ---- UDS (ISO 14229) client, tunnelled through CMD_UDS over any transport ----
def uds_key_from_seed(seed):
    k = ((seed << 3) | (seed >> 29)) & 0xFFFFFFFF   # rotate left 3, must match the FBL
    return (k ^ 0x5A3C96E1) & 0xFFFFFFFF


def uds_req(ser, pdu, desc):
    """Send one UDS request; return the response PDU, or None on failure (and print why)."""
    ok, p = transact(ser, CMD_UDS, bytes(pdu))
    if not ok:
        print(f"{desc}: no/framing-level reply"); return None
    if len(p) >= 3 and p[0] == 0x7F:
        print(f"{desc}: negative response (service 0x{p[1]:02X}, NRC 0x{p[2]:02X})"); return None
    return p


def udsinfo(ser):
    """Read a few Data Identifiers to prove the UDS layer works."""
    dids = {0xF195: "bootloader version", 0xF186: "active session",
            0xF190: "installed app version", 0xFD00: "last BIST"}
    for did, name in dids.items():
        r = uds_req(ser, [0x22, did >> 8, did & 0xFF], f"RDBI {did:04X}")
        if r:
            print(f"  {name:22} (DID {did:04X}): {r[3:].hex(' ')}")


def udsflash(ser, path):
    """Full UDS reprogramming sequence: session -> unlock -> download -> install -> reset."""
    try:
        hdr = open(path + ".hdr", "rb").read()
    except FileNotFoundError:
        print(f"Missing header: {path}.hdr (sign it first)"); return
    if len(hdr) != 164:
        print("Bad header length"); return
    encrypted = bool((hdr[6] | (hdr[7] << 8)) & 0x0001)
    img = open(path + (".enc" if encrypted else ""), "rb").read()
    tag = "encrypted " if encrypted else ""

    if uds_req(ser, [0x10, 0x02], "session") is None: return          # programming session
    print("Programming session.")

    r = uds_req(ser, [0x27, 0x01], "requestSeed")                     # security access
    if r is None or len(r) < 6: return
    seed = (r[2] << 24) | (r[3] << 16) | (r[4] << 8) | r[5]
    key = uds_key_from_seed(seed)
    if uds_req(ser, [0x27, 0x02, (key >> 24) & 0xFF, (key >> 16) & 0xFF,
                     (key >> 8) & 0xFF, key & 0xFF], "sendKey") is None: return
    print("Unlocked.")

    size = len(img)
    dl = [0x34, 0x00, 0x44] + list(struct.pack(">I", SLOT_B)) + list(struct.pack(">I", size))
    r = uds_req(ser, dl, "requestDownload")                           # erase + set up
    if r is None: return
    print(f"Download accepted: {size} {tag}bytes into Slot B 0x{SLOT_B:08X}.")

    bsc, off, BLK = 1, 0, 64
    while off < size:
        chunk = img[off:off + BLK]
        if uds_req(ser, [0x36, bsc] + list(chunk), f"transferData @{off}") is None: return
        off += len(chunk); bsc = (bsc + 1) & 0xFF
        print(f"\rTransferred {off}/{size} bytes", end="", flush=True)
    print("\nTransferred.")

    if uds_req(ser, [0x37], "transferExit") is None: return           # request transfer exit

    r = uds_req(ser, [0x31, 0x01, 0xFF, 0x01] + list(hdr), "install routine")  # verify + promote
    if r is None: return
    if len(r) < 5 or r[4] != 1:
        print("Install routine reported failure (bad signature or older version)."); return
    print("Installed (signature + version OK).")

    uds_req(ser, [0x11, 0x01], "ECUReset")                            # reboot -> app
    print("ECU reset -> application launching.")


def get_version(ser):
    ok, p = transact(ser, CMD_GET_VER)
    if ok and len(p) == 4:
        print(f"Bootloader version -> vendor {p[0]}, v{p[1]}.{p[2]}.{p[3]}")
    else:
        print("Version request failed:", ok, p.hex(" "))


def flash(ser, path):
    # read the header first: it tells us whether the payload is encrypted, and
    # therefore whether we stage the plaintext .bin or the ciphertext .enc
    hdrpath = path + ".hdr"
    try:
        hdr = open(hdrpath, "rb").read()
    except FileNotFoundError:
        print(f"Missing header: {hdrpath} (run: sign_tool.py sign {path} <version> [enc] first)"); return
    if len(hdr) != 164:
        print(f"Bad header length {len(hdr)} (expected 164)"); return
    flags = hdr[6] | (hdr[7] << 8)   # <IBBH...: flags is the H at byte offset 6
    encrypted = bool(flags & 0x0001)

    payload_path = path + ".enc" if encrypted else path
    try:
        img = open(payload_path, "rb").read()
    except FileNotFoundError:
        print(f"Missing staged payload: {payload_path}"); return

    npages = (len(img) + PAGE - 1) // PAGE
    tag = "encrypted " if encrypted else ""
    print(f"Image: {len(img)} {tag}bytes -> {npages} page(s), staging into Slot B 0x{SLOT_B:08X}")

    # 1) erase the staging slot (the running app in Slot A is left untouched)
    ok, p = transact_retry(ser, CMD_ERASE, struct.pack("<IB", SLOT_B, npages))
    if not ok_reply(ok, p):
        print("ERASE failed:", ok, p.hex(" ")); return
    print("Erased.")

    # 2) write in chunks
    for off in range(0, len(img), CHUNK):
        chunk = img[off:off + CHUNK]
        payload = struct.pack("<I", SLOT_B + off) + bytes([len(chunk)]) + chunk
        ok, p = transact_retry(ser, CMD_WRITE, payload)
        if not ok_reply(ok, p):
            print(f"\nWRITE failed at +0x{off:X}:", ok, p.hex(" ")); return
        print(f"\rWritten {off + len(chunk)}/{len(img)} bytes", end="", flush=True)
    print("\nProgrammed.")

    # 3) send the signed header; the board verifies it (signature + version),
    #    then decrypts into Slot A if the image is encrypted, and activates
    ok, p = transact_retry(ser, CMD_VERIFY, hdr)
    if not ok_reply(ok, p):
        print("VERIFY failed - rejected (bad signature, wrong type, or older version):", ok, p.hex(" ")); return
    print("Verified (signature + version OK)." + (" Decrypted into Slot A." if encrypted else ""))

    # 4) launch
    ok, p = transact(ser, CMD_GO)
    print("Launching application..." if ok else f"GO failed: {p.hex(' ')}")


def update_fbl(ser, path):
    img = open(path, "rb").read()
    try:
        hdr = open(path + ".hdr", "rb").read()
    except FileNotFoundError:
        print(f"Missing header: {path}.hdr (sign the FBL .bin with type fbl first)"); return
    if len(hdr) != 164:
        print("Bad header length"); return
    print(f"New FBL: {len(img)} bytes -> staging into Slot B 0x{SLOT_B:08X}")

    ok, p = transact(ser, CMD_ERASE, struct.pack("<IB", SLOT_B, 40))   # erase the whole 40 KB staging slot
    if not ok_reply(ok, p):
        print("ERASE failed:", ok, p.hex(" ")); return
    print("Erased.")

    for off in range(0, len(img), CHUNK):
        chunk = img[off:off + CHUNK]
        payload = struct.pack("<I", SLOT_B + off) + bytes([len(chunk)]) + chunk
        ok, p = transact(ser, CMD_WRITE, payload)
        if not ok_reply(ok, p):
            print("WRITE failed at offset", off); return
    print("Staged", len(img), "bytes.")

    ok, p = transact(ser, CMD_UPDATE_FBL, hdr)
    if ok_reply(ok, p):
        print("FBL update accepted - the board is reprogramming its own bootloader and will reset.")
        print("Wait a few seconds, then hold B1 at reset and run:  bl_host.py <PORT>")
    else:
        print("FBL update REJECTED (bad signature, wrong type, or older version):", ok, p.hex(" "))


def lock_bm(ser):
    print("This write-protects the Boot Manager (flash pages 0-15).")
    print("After this the BM cannot be reflashed until you remove WRP in STM32CubeProgrammer.")
    if input("Type LOCK to proceed: ").strip() != "LOCK":
        print("Aborted."); return
    ok, p = transact(ser, CMD_LOCK_BM)
    if ok_reply(ok, p):
        print("Accepted - the board is applying write protection and will reset.")
    elif not ok:
        # OB_Launch resets before replying is unusual (we defer it), but tolerate no-reply
        print("No ACK returned; if the board reset, WRP was applied. Verify in CubeProgrammer.")
    else:
        print("LOCK_BM REJECTED (option-byte program failed):", p.hex(" "))


def main():
    if len(sys.argv) < 2:
        print("usage: bl_host.py <PORT|tcp:host:port> [flash <file.bin>]"); return
    ser = open_transport(sys.argv[1])

    if len(sys.argv) >= 4 and sys.argv[2] == "flash":
        flash(ser, sys.argv[3])
    elif len(sys.argv) >= 4 and sys.argv[2] == "updatefbl":
        update_fbl(ser, sys.argv[3])
    elif len(sys.argv) >= 3 and sys.argv[2] == "lockbm":
        lock_bm(ser)
    elif len(sys.argv) >= 3 and sys.argv[2] == "bist":
        bist(ser)
    elif len(sys.argv) >= 3 and sys.argv[2] == "udsinfo":
        udsinfo(ser)
    elif len(sys.argv) >= 4 and sys.argv[2] == "udsflash":
        udsflash(ser, sys.argv[3])
    else:
        get_version(ser)

    ser.close()


if __name__ == "__main__":
    main()
