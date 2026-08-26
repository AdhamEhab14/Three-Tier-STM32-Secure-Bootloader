#!/usr/bin/env python3
"""
Author: Adham Ehab   Date: 18/08/2026

Firmware signing tool (Ed25519).

  python sign_tool.py genkey                     -> create a signing key pair, print public key as C
  python sign_tool.py genenckey                  -> create the ChaCha20 key, print it as C
  python sign_tool.py sign app.bin 1.0.0         -> write app.bin.hdr (signed, plaintext)
  python sign_tool.py sign app.bin 1.0.0 enc     -> signed + encrypted (writes app.bin.hdr + app.bin.enc)
  python sign_tool.py sign fbl.bin 1.3.0 fbl     -> sign a new bootloader image

The PRIVATE signing key (keys/bl_private.bin) and the ChaCha20 key
(keys/bl_enckey.bin) stay on your PC and are git-ignored. The PUBLIC signing key
and the ChaCha20 key are baked into the bootloader firmware.

Requires: pip install pynacl
"""
import sys
import os
import time
import struct
import hashlib
import nacl.signing

IMG_MAGIC = 0x21474D49   # "IMG!" - must match IMG_MAGIC in bootloader.h
IMG_FLAG_ENCRYPTED = 0x0001

KEYS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "keys")
PRIV = os.path.join(KEYS, "bl_private.bin")
PUB  = os.path.join(KEYS, "bl_public.bin")
ENCK = os.path.join(KEYS, "bl_enckey.bin")   # pre-shared ChaCha20 key (git-ignored)


# ---- ChaCha20 (RFC 8439), matches ChaCha20_Block in the firmware ----
def _rotl(a, b):
    return ((a << b) | (a >> (32 - b))) & 0xffffffff

def _chacha_block(key, counter, nonce):
    c = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574]
    st = c + list(struct.unpack("<8I", key)) + [counter] + list(struct.unpack("<3I", nonce))
    x = st[:]
    for _ in range(10):
        for a, b, cc, d in ((0,4,8,12),(1,5,9,13),(2,6,10,14),(3,7,11,15),
                            (0,5,10,15),(1,6,11,12),(2,7,8,13),(3,4,9,14)):
            x[a]=(x[a]+x[b])&0xffffffff; x[d]=_rotl(x[d]^x[a],16)
            x[cc]=(x[cc]+x[d])&0xffffffff; x[b]=_rotl(x[b]^x[cc],12)
            x[a]=(x[a]+x[b])&0xffffffff; x[d]=_rotl(x[d]^x[a],8)
            x[cc]=(x[cc]+x[d])&0xffffffff; x[b]=_rotl(x[b]^x[cc],7)
    return struct.pack("<16I", *[(x[i] + st[i]) & 0xffffffff for i in range(16)])

def chacha20_xor(key, nonce, data):
    out = bytearray(len(data))
    for off in range(0, len(data), 64):
        ks = _chacha_block(key, off // 64, nonce)
        for i in range(min(64, len(data) - off)):
            out[off + i] = data[off + i] ^ ks[i]
    return bytes(out)


def genenckey():
    if os.path.exists(ENCK):
        print("Refusing to overwrite existing key:", ENCK)
        return
    os.makedirs(KEYS, exist_ok=True)
    key = os.urandom(32)
    open(ENCK, "wb").write(key)
    print("Wrote", ENCK)
    print("\n--- paste this into the bootloader firmware (BL_ENC_KEY) ---")
    print("static const uint8_t BL_ENC_KEY[32] = {")
    for i in range(0, 32, 8):
        print("    " + ", ".join(f"0x{b:02X}" for b in key[i:i+8]) + ("," if i < 24 else ""))
    print("};")


def genkey():
    if os.path.exists(PRIV):
        print("Refusing to overwrite existing key:", PRIV)
        return
    os.makedirs(KEYS, exist_ok=True)
    sk = nacl.signing.SigningKey.generate()
    seed = bytes(sk)                 # 32-byte private seed
    pub  = bytes(sk.verify_key)      # 32-byte public key
    open(PRIV, "wb").write(seed)
    open(PUB,  "wb").write(pub)
    print("Wrote", PRIV, "and", PUB)

    cbytes = ", ".join(f"0x{b:02X}" for b in pub)
    print("\n--- paste this into the bootloader firmware ---")
    print("static const uint8_t BL_PUBLIC_KEY[32] = {")
    print("    " + cbytes)
    print("};")


def sign(path, version, img_type, encrypt):
    sk = nacl.signing.SigningKey(open(PRIV, "rb").read())
    payload = open(path, "rb").read()

    if encrypt:
        enckey = open(ENCK, "rb").read()
        nonce  = os.urandom(12)
        staged = chacha20_xor(enckey, nonce, payload)  # what actually lands in Slot B
        flags  = IMG_FLAG_ENCRYPTED
        open(path + ".enc", "wb").write(staged)
    else:
        nonce  = b"\x00" * 12
        staged = payload
        flags  = 0

    digest = hashlib.sha512(staged).digest()           # SHA-512 of the staged bytes

    major, minor, patch = (int(x) for x in version.split("."))
    fw_version = (major << 16) | (minor << 8) | patch
    type_code  = 1 if img_type == "app" else 2

    # 100-byte header (must match img_header_t): magic, type, hdr_ver, flags,
    # fw_version, build_time, payload_size, reserved, 12-byte nonce, 64-byte digest.
    header = struct.pack("<IBBHIIII",
                         IMG_MAGIC, type_code, 1, flags,
                         fw_version, int(time.time()), len(payload), 0) + nonce + digest
    sig = sk.sign(hashlib.sha512(header).digest()).signature   # sign the header itself
    open(path + ".hdr", "wb").write(header + sig)              # 100 + 64 = 164 bytes
    tag = "encrypted " if encrypt else ""
    print(f"Signed {len(payload)} {tag}bytes as {img_type} v{version} -> {path}.hdr"
          + (f" (+ {path}.enc)" if encrypt else ""))


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "genkey":
        genkey()
    elif len(sys.argv) >= 2 and sys.argv[1] == "genenckey":
        genenckey()
    elif len(sys.argv) >= 4 and sys.argv[1] == "sign":
        opts = [a.lower() for a in sys.argv[4:]]
        img_type = "fbl" if "fbl" in opts else "app"
        encrypt  = "enc" in opts
        sign(sys.argv[2], sys.argv[3], img_type, encrypt)
    else:
        print("usage: sign_tool.py genkey | genenckey | "
              "sign <file.bin> <version> [app|fbl] [enc]")


if __name__ == "__main__":
    main()
