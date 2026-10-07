"""SecurityAccess key derivation for the host tools (UDS service 0x27).

The board answers a 4-byte seed with the first 4 bytes of AES-CMAC (RFC 4493) of the
seed under a 128-bit key built into the firmware. This file is the host's side of that:
a small pure-Python AES-128 and CMAC, so the tools need no extra dependency, plus the
rule for finding the key.

Which key:
  1. $BL_SEC_KEY_FILE, a 16-byte file, else
  2. bl_seckey.bin in $BL_KEYS_DIR or host/keys (python sign_tool.py genseckey makes it), else
  3. the public demo key, which is what a firmware built without its own key contains.
"""
import os

DEMO_KEY = bytes.fromhex("44656d6f2d4b65792d5075626c696321")   # "Demo-Key-Public!"

_HERE = os.path.dirname(os.path.abspath(__file__))


def _sbox():
    def xtime(a):
        a <<= 1
        return (a ^ 0x11B) & 0xFF if a & 0x100 else a

    def mul(a, b):
        r = 0
        while b:
            if b & 1:
                r ^= a
            a = xtime(a)
            b >>= 1
        return r

    box = []
    for i in range(256):
        b = next((x for x in range(1, 256) if mul(i, x) == 1), 0)
        s = b
        for n in (1, 2, 3, 4):
            s ^= ((b << n) | (b >> (8 - n))) & 0xFF
        box.append(s ^ 0x63)
    return box


_SBOX = _sbox()


def _xtime(x):
    return ((x << 1) ^ 0x1B) & 0xFF if x & 0x80 else (x << 1) & 0xFF


def _expand(key):
    rk = list(key)
    rcon = 1
    for i in range(16, 176, 4):
        t = rk[i - 4:i]
        if i % 16 == 0:
            t = [_SBOX[t[1]] ^ rcon, _SBOX[t[2]], _SBOX[t[3]], _SBOX[t[0]]]
            rcon = _xtime(rcon)
        rk += [rk[i - 16 + k] ^ t[k] for k in range(4)]
    return rk


def aes128_encrypt(key, block):
    """One AES-128 block (FIPS-197)."""
    rk = _expand(key)
    s = [b ^ k for b, k in zip(block, rk[:16])]
    for rnd in range(1, 11):
        t = [_SBOX[s[4 * ((i // 4 + i % 4) % 4) + i % 4]] for i in range(16)]
        if rnd != 10:
            s = []
            for c in range(4):
                a = t[4 * c:4 * c + 4]
                allx = a[0] ^ a[1] ^ a[2] ^ a[3]
                s += [a[k] ^ allx ^ _xtime(a[k] ^ a[(k + 1) % 4]) for k in range(4)]
        else:
            s = t
        s = [b ^ k for b, k in zip(s, rk[16 * rnd:16 * rnd + 16])]
    return bytes(s)


def _dbl(b):
    v = int.from_bytes(b, "big") << 1
    if v >> 128:
        v = (v & ((1 << 128) - 1)) ^ 0x87
    return v.to_bytes(16, "big")


def aes_cmac(key, msg):
    """AES-CMAC (RFC 4493)."""
    k1 = _dbl(aes128_encrypt(key, bytes(16)))
    k2 = _dbl(k1)
    n = max(1, (len(msg) + 15) // 16)
    complete = len(msg) > 0 and len(msg) % 16 == 0
    x = bytes(16)
    for i in range(n - 1):
        x = aes128_encrypt(key, bytes(a ^ b for a, b in zip(x, msg[16 * i:16 * i + 16])))
    last = msg[16 * (n - 1):]
    if complete:
        last = bytes(a ^ b for a, b in zip(last, k1))
    else:
        last = last + b"\x80" + bytes(15 - len(last))
        last = bytes(a ^ b for a, b in zip(last, k2))
    return aes128_encrypt(key, bytes(a ^ b for a, b in zip(x, last)))


def load_key():
    path = os.environ.get("BL_SEC_KEY_FILE")
    if not path:
        keys = os.environ.get("BL_KEYS_DIR") or os.path.join(_HERE, "keys")
        cand = os.path.join(keys, "bl_seckey.bin")
        path = cand if os.path.exists(cand) else None
    if path:
        key = open(path, "rb").read()
        if len(key) != 16:
            raise ValueError("%s must be exactly 16 bytes" % path)
        return key
    return DEMO_KEY


def key_for_seed(seed, key=None):
    """The 4-byte answer to a 4-byte seed: first 4 bytes of AES-CMAC(K, seed)."""
    seed = bytes(seed)
    if len(seed) != 4:
        raise ValueError("a SecurityAccess seed is 4 bytes")
    return aes_cmac(key or load_key(), seed)[:4]
