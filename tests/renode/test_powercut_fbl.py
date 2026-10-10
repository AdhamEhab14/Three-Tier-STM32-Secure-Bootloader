"""Power cut during the bootloader's own self-update.

The updater erases the whole 40 KB FBL region and then copies the new image over from
Slot B, so a cut after the first erase leaves a half-written FBL. The Boot Manager has to
bring the device back from each of those states.

Run it (no board needed; needs Renode and the build-test firmware, see docs/testing.md
section 6 for the one-time build):

    cd tests/renode
    python -m pytest -v test_powercut_fbl.py

This is one of the slow power-cut files (marked powercut). Set BL_THOROUGH=1 for a denser sweep
of cut points.
"""
import struct

import pytest

import bl_host
import powercut as pc
from conftest import ROOT, BUILD
from renode_session import RenodeSerial

import os

BM_STATE_MAGIC = 0xB007F00D
BM_FBL_UPDATING = 2

pytestmark = pytest.mark.powercut


@pytest.fixture(scope="module")
def sbl_run(session):
    """Stage a new FBL, start the update, and stop the plug just before the SBL's first erase."""
    ser = RenodeSerial(session, timeout=5)
    fbl = open(os.path.join(ROOT, BUILD, "fbl.bin"), "rb").read()
    new_fbl = fbl + b"\xC3" * 16                  # a different, still bootable, image
    rel, hdr = pc.make_signed("fbl_new", new_fbl, "2.1.0", "fbl")
    pc.stage(session, ser, rel, 40)
    pre = session.dump_flash()

    ops_before = session.flash_ops()
    session.set_cut_on_erase(pc.FBL_BASE)
    ser.write(bl_host.build_frame(bl_host.CMD_UPDATE_FBL, hdr))
    pc.run_until_cut(session)
    assert session.cut_info() == (1, pc.FBL_BASE), "the first erase the SBL starts should be the FBL region"
    start = session.dump_flash()
    return {
        "pre": pre,
        "start": start,
        "hdr": hdr,
        "slot_b": start[pc.off(pc.SLOT_B):pc.off(pc.SLOT_B) + pc.FBL_SIZE],
        "ops_to_sbl": session.flash_ops() - ops_before,
        "ops_before": ops_before,
    }


def test_cut_point_is_the_expected_one(sbl_run):
    magic, crc, state = struct.unpack_from("<III", sbl_run["start"], pc.off(pc.BM_STATE))
    assert magic == BM_STATE_MAGIC and state == BM_FBL_UPDATING
    old_fbl = sbl_run["pre"][pc.off(pc.FBL_BASE):pc.off(pc.FBL_BASE) + pc.FBL_SIZE]
    now_fbl = sbl_run["start"][pc.off(pc.FBL_BASE):pc.off(pc.FBL_BASE) + pc.FBL_SIZE]
    assert old_fbl == now_fbl, "nothing in the FBL region should have changed yet"


def test_generated_states_match_a_real_cut_in_the_copy_loop(session, sbl_run):
    """Cut the real SBL 100 halfwords into the copy and compare with the generated state."""
    j = 100
    ser = RenodeSerial(session, timeout=5)
    session.restore_flash(sbl_run["pre"], pc.WORK_REL + "/pre.bin")
    session.power_cycle()
    base = session.flash_ops()
    session.set_cut(base + sbl_run["ops_to_sbl"] + 40 + j)
    ser.write(bl_host.build_frame(bl_host.CMD_UPDATE_FBL, sbl_run["hdr"]))
    pc.run_until_cut(session)
    real = session.dump_flash()
    new = sbl_run["slot_b"]
    # the cut lands on the PG of halfword j; the CPU may or may not have written it yet
    candidates = [pc.sbl_state(sbl_run["start"], new, 40, j + d) for d in (0, 1)]
    if real not in candidates:
        gen = candidates[0]
        diffs = [i for i in range(len(real)) if real[i] != gen[i]]
        regions = sorted({(pc.FLASH_BASE + i) & ~(pc.PAGE - 1) for i in diffs})
        fbl = real[pc.off(pc.FBL_BASE):pc.off(pc.FBL_BASE) + pc.FBL_SIZE]
        copied = 0
        while copied < pc.FBL_SIZE // 2 and fbl[2 * copied:2 * copied + 2] == new[2 * copied:2 * copied + 2]:
            copied += 1
        raise AssertionError(
            "real cut differs from generated state in %d bytes over pages %s; "
            "real FBL region has %d leading halfwords equal to Slot B (expected about %d)"
            % (len(diffs), [hex(r) for r in regions[:8]], copied, j))


# Each case costs about a minute (the Boot Manager redoes a 40 KB copy). The default set
# covers every kind of state once; BL_THOROUGH=1 runs the denser sweep.
QUICK = [
    ("erased-0-pages", dict(pages_erased=0)),
    ("erased-20-pages", dict(pages_erased=20)),
    ("erased-40-pages", dict(pages_erased=40)),
    ("copied-1000-halfwords", dict(pages_erased=40, halfwords_copied=1000)),
    ("torn-erase-half-page-0", dict(pages_erased=0, torn_page=(0, "half"))),
    ("torn-erase-garbage-page-39", dict(pages_erased=39, torn_page=(39, "garbage"))),
]
FULL = (
    [("erased-%d-pages" % k, dict(pages_erased=k)) for k in (1, 39)]
    + [("copied-%d-halfwords" % j, dict(pages_erased=40, halfwords_copied=j)) for j in (1, 10240, 20479)]
    + [("torn-erase-garbage-page-0", dict(pages_erased=0, torn_page=(0, "garbage"))),
       ("torn-erase-half-page-20", dict(pages_erased=20, torn_page=(20, "half")))]
)
CASES = QUICK + (FULL if os.environ.get("BL_THOROUGH") else [])


@pytest.mark.parametrize("name,spec", CASES, ids=[c[0] for c in CASES])
def test_device_recovers_from_a_cut(session, sbl_run, name, spec):
    state = pc.sbl_state(sbl_run["start"], sbl_run["slot_b"], **spec)
    session.restore_flash(state, pc.WORK_REL + "/state.bin")
    session.power_cycle()
    ser = RenodeSerial(session, timeout=2)
    assert pc.alive(session, ser), "the device never came back after a power cut at: " + name
    # the new FBL ran, so the plaintext copy in Slot B (it carries the keys) is gone
    assert session.read_word(pc.SLOT_B) == 0xFFFFFFFF, "Slot B still holds the FBL after: " + name
    assert session.read_word(pc.SLOT_B + pc.FBL_SIZE - 4) == 0xFFFFFFFF
