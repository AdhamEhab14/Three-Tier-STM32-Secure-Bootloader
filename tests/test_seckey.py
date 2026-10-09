"""SecurityAccess crypto: the host's Python AES-CMAC against the standard vectors, and
against the firmware's own C code (compiled natively) on random inputs.

Nothing here needs a board or an emulator, only a C compiler for the cross-check
(the cross-check tests skip cleanly without one).

Run it (no board needed; from the repository root):

    cd tests
    python -m pytest -v test_seckey.py

The C parts are compiled with a host gcc on the PATH and skipped without one.
"""
import os
import random
import re
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "host"))

import seckey  # noqa: E402

CORE = os.path.join(ROOT, "STM32F103RBT6_Secure_Bootloader", "Core")
GCC = shutil.which("gcc")

# RFC 4493 section 4
KEY = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
MSG = bytes.fromhex(
    "6bc1bee22e409f96e93d7e117393172a"
    "ae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52ef"
    "f69f2445df4f9b17ad2b417be66c3710")
VECTORS = {
    0: "bb1d6929e95937287fa37d129b756746",
    16: "070a16b46b4d4144f79bdd9dd04a287c",
    40: "dfa66747de9ae63030ca32611497c827",
    64: "51f0bebf7e3b9d92fc49741779363cfe",
}


def test_aes128_fips197_vector():
    key = bytes(range(16))
    plain = bytes.fromhex("00112233445566778899aabbccddeeff")
    assert seckey.aes128_encrypt(key, plain).hex() == "69c4e0d86a7b0430d8cdb78070b4c55a"


@pytest.mark.parametrize("n", sorted(VECTORS))
def test_cmac_rfc4493_vectors(n):
    assert seckey.aes_cmac(KEY, MSG[:n]).hex() == VECTORS[n]


def test_key_for_seed_is_four_bytes_and_depends_on_the_seed():
    k1 = seckey.key_for_seed(b"\x01\x02\x03\x04", seckey.DEMO_KEY)
    k2 = seckey.key_for_seed(b"\x01\x02\x03\x05", seckey.DEMO_KEY)
    assert len(k1) == 4 and k1 != k2


def test_key_is_not_a_simple_function_of_the_seed():
    """The old scheme was key = seed XOR constant, so two pairs gave the constant away."""
    seeds = [bytes([i, 2 * i, 3 * i, 7 * i + 1]) for i in range(1, 9)]
    consts = {bytes(a ^ b for a, b in zip(s, seckey.key_for_seed(s, seckey.DEMO_KEY))) for s in seeds}
    assert len(consts) == len(seeds), "key XOR seed is constant: the key is trivially recoverable"


def test_a_wrong_length_seed_is_rejected():
    with pytest.raises(ValueError):
        seckey.key_for_seed(b"\x00\x01\x02")


# ---- cross-check against the firmware C code -----------------------------------------

@pytest.fixture(scope="module")
def native(tmp_path_factory):
    """seccrypto_cli built twice: with the demo key and with the published test key."""
    if not GCC:
        pytest.skip("no C compiler")
    out = tmp_path_factory.mktemp("native")
    exes = {}
    for name, defs in (("demo", []), ("test", ["-DBL_TEST_KEYS", "-I" + os.path.join(ROOT, "tests", "keys")])):
        exe = str(out / ("cli_" + name))
        cmd = [GCC, "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-cpp", *defs, "-I" + os.path.join(CORE, "Inc"),
               os.path.join(ROOT, "tests", "native", "seccrypto_cli.c"),
               os.path.join(CORE, "Src", "bl_seccrypto.c"), "-o", exe]
        subprocess.run(cmd, check=True)
        exes[name] = exe
    return exes


def run(exe, *args):
    return subprocess.run([exe, *args], check=True, capture_output=True, text=True).stdout.strip()


def test_firmware_c_passes_its_own_vectors(native):
    out = subprocess.run([native["demo"]], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout


def test_c_and_python_cmac_agree_on_random_inputs(native):
    rng = random.Random(4493)
    for _ in range(60):
        key = bytes(rng.randrange(256) for _ in range(16))
        msg = bytes(rng.randrange(256) for _ in range(rng.randrange(0, 70)))
        assert run(native["demo"], "cmac", key.hex(), msg.hex() or "-") == seckey.aes_cmac(key, msg).hex()


def test_c_and_python_agree_on_the_demo_key_response(native):
    rng = random.Random(27)
    for _ in range(40):
        seed = bytes(rng.randrange(256) for _ in range(4))
        assert run(native["demo"], "keyforseed", seed.hex()) == seckey.key_for_seed(seed, seckey.DEMO_KEY).hex()


def test_c_and_python_agree_on_the_test_key_response(native):
    key = open(os.path.join(ROOT, "tests", "keys", "bl_seckey.bin"), "rb").read() \
        if os.path.exists(os.path.join(ROOT, "tests", "keys", "bl_seckey.bin")) else None
    if key is None:
        subprocess.run([sys.executable, os.path.join(ROOT, "tests", "keys", "make_test_keys.py")], check=True)
        key = open(os.path.join(ROOT, "tests", "keys", "bl_seckey.bin"), "rb").read()
    rng = random.Random(28)
    for _ in range(20):
        seed = bytes(rng.randrange(256) for _ in range(4))
        assert run(native["test"], "keyforseed", seed.hex()) == seckey.key_for_seed(seed, key).hex()


def test_the_demo_key_in_the_header_matches_the_host_constant():
    text = open(os.path.join(CORE, "Inc", "bl_seckey_demo.h")).read()
    bytes_ = bytes(int(t, 16) for t in re.findall(r"0x([0-9A-Fa-f]{2})", text))
    assert bytes_ == seckey.DEMO_KEY


def test_secaccess_policy_unit_tests_pass(tmp_path):
    if not GCC:
        pytest.skip("no C compiler")
    exe = str(tmp_path / "secaccess_test")
    subprocess.run([GCC, "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-cpp", "-I" + os.path.join(CORE, "Inc"),
                    os.path.join(ROOT, "tests", "native", "secaccess_test.c"),
                    os.path.join(CORE, "Src", "bl_secaccess.c"), "-o", exe], check=True)
    out = subprocess.run([exe], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout


# ---- the CAPL port ---------------------------------------------------------------------

CAPL_MAIN = r'''
#include <stdio.h>
#include <stdlib.h>

static int unhex(const char *s, byte *out)
{
    int n = 0;
    if (s[0] == '-') return 0;
    while (s[0] && s[1]) { unsigned v; sscanf(s, "%2x", &v); out[n++] = (byte)v; s += 2; }
    return n;
}

int main(int argc, char **argv)
{
    byte key[16], msg[80], mac[16];
    int i, n;

    if (argc == 4) {                      /* cmac <key> <msg> */
        unhex(argv[2], key);
        n = unhex(argv[3], msg);
        aesCmac(key, msg, n, mac);
    } else {                              /* keyforseed <seed>: uses gSecKey */
        unhex(argv[2], msg);
        keyForSeed(msg, mac);
        for (i = 0; i < 4; i++) printf("%02x", mac[i]);
        printf("\n");
        return 0;
    }
    for (i = 0; i < 16; i++) printf("%02x", mac[i]);
    printf("\n");
    return 0;
}
'''


@pytest.fixture(scope="module")
def capl_exe(tmp_path_factory):
    """seckey.cin turned into plain C (the CAPL `variables { }` wrapper removed, `byte` defined)."""
    if not GCC:
        pytest.skip("no C compiler")
    text = open(os.path.join(ROOT, "diagnostics", "canoe", "seckey.cin"), encoding="latin-1").read()
    text = text.replace("\r\n", "\n")
    text = text.replace("variables\n{\n", "", 1)
    text = text.replace("\n}\n", "\n", 1)             # closes the variables block
    out = tmp_path_factory.mktemp("capl")
    (out / "capl.c").write_text("typedef unsigned char byte;\n" + text + CAPL_MAIN)
    exe = str(out / "capl")
    subprocess.run([GCC, "-O1", "-Wall", str(out / "capl.c"), "-o", exe], check=True)
    return exe


def test_capl_port_matches_the_rfc_vectors(capl_exe):
    for n, want in VECTORS.items():
        assert run(capl_exe, "cmac", KEY.hex(), MSG[:n].hex() or "-") == want


def test_capl_port_matches_python_on_random_inputs(capl_exe):
    rng = random.Random(1229)
    for _ in range(40):
        key = bytes(rng.randrange(256) for _ in range(16))
        msg = bytes(rng.randrange(256) for _ in range(rng.randrange(0, 65)))
        assert run(capl_exe, "cmac", key.hex(), msg.hex() or "-") == seckey.aes_cmac(key, msg).hex()


def test_capl_key_for_seed_matches_the_demo_key_response(capl_exe):
    rng = random.Random(1230)
    for _ in range(20):
        seed = bytes(rng.randrange(256) for _ in range(4))
        assert run(capl_exe, "keyforseed", seed.hex()) == seckey.key_for_seed(seed, seckey.DEMO_KEY).hex()
