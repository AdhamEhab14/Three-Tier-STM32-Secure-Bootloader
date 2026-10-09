"""Encrypted images: the board decrypts them on install, and refuses one it cannot decrypt.

The signature covers the ciphertext, so an image encrypted under a different key (an old key
after a rotation, say) is still correctly signed. The header carries the CRC of the plaintext
(format 2) and the board checks it before it erases the working app.

Run it (no board needed; needs Renode and the build-test firmware, see docs/testing.md
section 6 for the one-time build):

    cd tests/renode
    python -m pytest -v test_encrypted_install.py
"""
import os
import struct

import pytest

import bl_host
import sign_tool
from conftest import ROOT, BUILD

WORK_REL = "tests/renode/_work"
WORK = os.path.join(ROOT, "tests", "renode", "_work")
SLOT_A = 0x0800E000
SLOT_B = 0x08015000


@pytest.fixture(scope="module")
def app_bin():
    return open(os.path.join(ROOT, BUILD, "application.bin"), "rb").read()


def make_encrypted(name, payload, version):
    """Sign and encrypt payload; return (relative path of the .enc, header bytes)."""
    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, name + ".bin")
    open(path, "wb").write(payload)
    sign_tool.sign(path, version, "app", True)
    return "%s/%s.bin.enc" % (WORK_REL, name), open(path + ".hdr", "rb").read()


def install(session, ser, rel_enc, hdr):
    """Stage the ciphertext in Slot B and send the real VERIFY. True if the board accepted it."""
    ok, p = bl_host.transact(ser, bl_host.CMD_ERASE, struct.pack("<IB", SLOT_B, 4))
    assert bl_host.ok_reply(ok, p), "erase of staging slot failed"
    session.load_binary(rel_enc, SLOT_B)
    ok, p = bl_host.transact(ser, bl_host.CMD_VERIFY, hdr)
    return bl_host.ok_reply(ok, p)


def slot_a(session, count=8):
    return [session.read_word(SLOT_A + 4 * i) for i in range(count)]


def test_an_encrypted_image_installs_as_plaintext(session, ser, app_bin):
    payload = app_bin + b"\x31" * 16
    rel, hdr = make_encrypted("enc_ok", payload, "3.0.0")
    assert install(session, ser, rel, hdr), "a correctly encrypted image was refused"
    assert session.read_word(SLOT_A) == struct.unpack("<I", payload[:4])[0], "Slot A does not hold the plaintext"
    assert session.read_word(SLOT_A + len(app_bin)) == 0x31313131


def test_an_image_encrypted_under_another_key_is_refused(session, ser, app_bin, monkeypatch, tmp_path):
    other = tmp_path / "other_enckey.bin"
    other.write_bytes(os.urandom(32))
    monkeypatch.setattr(sign_tool, "ENCK", str(other))      # still signed with the right key
    rel, hdr = make_encrypted("enc_wrong_key", app_bin + b"\x32" * 16, "3.1.0")
    before = slot_a(session)
    assert not install(session, ser, rel, hdr), "garbage from a wrong key was installed"
    assert slot_a(session) == before, "the working app was touched"


def test_an_encrypted_image_without_the_plaintext_crc_is_refused(session, ser, app_bin, monkeypatch):
    monkeypatch.setattr(sign_tool, "HDR_VERSION", 1)          # the old header format
    monkeypatch.setattr(sign_tool, "plain_crc", lambda data: 0)
    rel, hdr = make_encrypted("enc_v1", app_bin + b"\x33" * 16, "3.2.0")
    before = slot_a(session)
    assert not install(session, ser, rel, hdr), "an encrypted v1 image was accepted"
    assert slot_a(session) == before
