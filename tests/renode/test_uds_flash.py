"""The UDS reprogramming flow of the production bootloader, start to finish.

bl_host.udsflash does what a diagnostic tester does: programming session, SecurityAccess,
RequestDownload, TransferData, TransferExit, the install routine, ECUReset. It is run here
unmodified against the real firmware.

Run it (no board needed; needs Renode and the build-test firmware, see docs/testing.md
section 6 for the one-time build):

    cd tests/renode
    python -m pytest -v test_uds_flash.py
"""
import os
import struct

import bl_host
import powercut as pc
from conftest import ROOT, BUILD


def test_udsflash_installs_a_signed_app(session, ser, capsys):
    app = open(os.path.join(ROOT, BUILD, "application.bin"), "rb").read()
    rel, _ = pc.make_signed("udsflash_app", app, "1.0.0", "app")
    bl_host.udsflash(ser, os.path.join(ROOT, rel))
    out = capsys.readouterr().out
    assert "Unlocked." in out, out
    assert "Installed (signature + version OK)." in out, out
    assert session.read_word(pc.SLOT_A) == struct.unpack("<I", app[:4])[0]
