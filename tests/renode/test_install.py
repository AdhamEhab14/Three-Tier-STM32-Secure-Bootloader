"""Signed-image install, anti-rollback and tamper rejection on the real firmware.

The first test does a complete install over the emulated UART with the real host
tool. The rest stage the image straight into Slot B (the same bytes the UART path
would write, minus about two minutes of byte-by-byte injection) and send the real
VERIFY command, so the signature, digest and version checks are the firmware's own.

Run it (no board needed; needs Renode and the build-test firmware, see docs/testing.md
section 6 for the one-time build):

    cd tests/renode
    python -m pytest -v test_install.py
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


def make_image(name, payload, version):
    """Write payload to _work/<name>.bin, sign it, return (relative path, header bytes)."""
    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, name + ".bin")
    open(path, "wb").write(payload)
    sign_tool.sign(path, version, "app", False)
    return "%s/%s.bin" % (WORK_REL, name), open(path + ".hdr", "rb").read()


def slot_a_words(session, count):
    return [session.read_word(SLOT_A + 4 * i) for i in range(count)]


def stage_and_verify(session, ser, rel_bin, hdr):
    """Erase Slot B, drop the payload in, send the real VERIFY. True if accepted."""
    npages = 4
    ok, p = bl_host.transact(ser, bl_host.CMD_ERASE, struct.pack("<IB", SLOT_B, npages))
    assert bl_host.ok_reply(ok, p), "erase of staging slot failed"
    session.load_binary(rel_bin, SLOT_B)
    ok, p = bl_host.transact(ser, bl_host.CMD_VERIFY, hdr)
    return bl_host.ok_reply(ok, p)


def test_signed_app_installs_over_uart(session, ser, app_bin, capsys):
    rel, _ = make_image("app_v1_0_0", app_bin, "1.0.0")
    bl_host.flash(ser, os.path.join(ROOT, rel))
    out = capsys.readouterr().out
    assert "Verified (signature + version OK)" in out, out
    first = struct.unpack("<I", app_bin[:4])[0]
    assert session.read_word(SLOT_A) == first, "Slot A does not hold the new application"
    session.reboot()


def test_older_version_is_refused(session, ser, app_bin):
    before = slot_a_words(session, 8)
    rel, hdr = make_image("app_v0_9_0", app_bin + b"\xA5" * 16, "0.9.0")
    assert not stage_and_verify(session, ser, rel, hdr), "anti-rollback let an older image in"
    assert slot_a_words(session, 8) == before, "Slot A changed after a refused image"


def test_bad_signature_is_refused(session, ser, app_bin):
    rel, hdr = make_image("app_badsig", app_bin + b"\x5A" * 16, "1.1.0")
    bad = bytearray(hdr)
    bad[-1] ^= 0x01                       # corrupt the last signature byte
    before = slot_a_words(session, 8)
    assert not stage_and_verify(session, ser, rel, bytes(bad)), "a corrupted signature was accepted"
    assert slot_a_words(session, 8) == before


def test_payload_that_does_not_match_the_header_is_refused(session, ser, app_bin):
    rel_ok, hdr = make_image("app_tamper_ok", app_bin + b"\x11" * 16, "1.1.0")
    rel_evil, _ = make_image("app_tamper_evil", app_bin + b"\x22" * 16, "1.1.0")
    before = slot_a_words(session, 8)
    assert not stage_and_verify(session, ser, rel_evil, hdr), "an image not covered by the signature was accepted"
    assert slot_a_words(session, 8) == before


def test_newer_signed_version_is_accepted(session, ser, app_bin):
    payload = app_bin + b"\x77" * 16
    rel, hdr = make_image("app_v1_1_0", payload, "1.1.0")
    assert stage_and_verify(session, ser, rel, hdr)
    tail = session.read_word(SLOT_A + len(app_bin))
    assert tail == 0x77777777, "the new image was not promoted into Slot A"
