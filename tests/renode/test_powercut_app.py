"""Power cut while installing an application.

An install erases the app pages, copies the app in, rewrites the metadata (installed
version, which is also the anti-rollback floor) and then the boot-trial page. After a cut
at any of those points the device must come back up in the bootloader and still refuse an
older image.
"""
import os

import pytest

import bl_host
import powercut as pc
from conftest import ROOT, BUILD
from renode_session import RenodeSerial

OLD_VERSION = "1.0.0"
NEW_VERSION = "1.1.0"
OLDER_VERSION = "0.9.0"

pytestmark = pytest.mark.powercut


@pytest.fixture(scope="module")
def app_run(session):
    """Install 1.0.0, stage 1.1.0, and capture the flash before, during and after the upgrade."""
    ser = RenodeSerial(session, timeout=5)
    app = open(os.path.join(ROOT, BUILD, "application.bin"), "rb").read()
    new_app = app + b"\x77" * 16
    assert pc.try_install(session, ser, "app_old", app, OLD_VERSION), "baseline install failed"

    rel, hdr = pc.make_signed("app_new", new_app, NEW_VERSION, "app")
    pc.stage(session, ser, rel, 4)
    pre = session.dump_flash()

    # a complete upgrade, for the reference "after" image
    ok, p = bl_host.transact(ser, bl_host.CMD_VERIFY, hdr)
    assert bl_host.ok_reply(ok, p)
    final = session.dump_flash()
    return {"app": app, "new_app": new_app, "pre": pre, "final": final, "hdr": hdr,
            "size": len(new_app)}


def test_the_reference_images_differ_where_expected(app_run):
    pre, final = app_run["pre"], app_run["final"]
    meta = pc.off(pc.meta_page_rewritten(pre, final))
    assert pre[meta:meta + 24] != final[meta:meta + 24], "the upgrade did not write a metadata record"
    # the record that was current before the upgrade must still be there afterwards
    other = pc.off(pc.CONFIG if pc.off(pc.CONFIG_ALT) == meta else pc.CONFIG_ALT)
    assert pre[other:other + 24] == final[other:other + 24]
    assert final[pc.off(pc.SLOT_A) + len(app_run["app"]):pc.off(pc.SLOT_A) + app_run["size"]] == b"\x77" * 16


def test_generated_state_matches_a_real_cut_before_the_metadata_rewrite(session, app_run):
    """Cut the real firmware just before it erases the metadata page."""
    ser = RenodeSerial(session, timeout=5)
    session.restore_flash(app_run["pre"], pc.WORK_REL + "/pre.bin")
    session.power_cycle()
    meta_page = pc.meta_page_rewritten(app_run["pre"], app_run["final"])
    session.set_cut_on_erase(meta_page)
    ser.write(bl_host.build_frame(bl_host.CMD_VERIFY, app_run["hdr"]))
    pc.run_until_cut(session)
    assert session.cut_info() == (1, meta_page)
    real = session.dump_flash()
    generated = pc.install_state(app_run["pre"], app_run["final"], app_run["size"],
                                 pages_erased=4, halfwords=(app_run["size"] + 1) // 2)
    assert real == generated, "the generated install state does not match a real cut"


NPAGES = 4
HW = None   # filled lazily from the image size inside the cases below

CASES = (
    [("erased-%d-app-pages" % k, dict(pages_erased=k)) for k in (1, 2, 4)]
    + [("copied-%d-halfwords" % j, dict(pages_erased=4, halfwords=j)) for j in (1, 500, 1500)]
    + [("torn-erase-half-app-page-0", dict(pages_erased=0, torn_page=(0, "half"))),
       ("torn-erase-garbage-app-page-2", dict(pages_erased=2, torn_page=(2, "garbage")))]
    + [("meta-erased", dict(pages_erased=4, halfwords=-1, meta_erased=True, meta_halfwords=0))]
    + [("meta-%d-halfwords" % m, dict(pages_erased=4, halfwords=-1, meta_erased=True, meta_halfwords=m))
       for m in (1, 2, 6, 10, 11)]
    + [("meta-done-trial-erased", dict(pages_erased=4, halfwords=-1, meta_erased=True,
                                       meta_halfwords=12, trial_erased=True))]
)


@pytest.mark.parametrize("name,spec", CASES, ids=[c[0] for c in CASES])
def test_device_recovers_and_floor_holds(session, app_run, name, spec):
    spec = dict(spec)
    if spec.get("halfwords") == -1:
        spec["halfwords"] = (app_run["size"] + 1) // 2
    state = pc.install_state(app_run["pre"], app_run["final"], app_run["size"], **spec)
    session.restore_flash(state, pc.WORK_REL + "/state.bin")
    session.power_cycle()
    ser = RenodeSerial(session, timeout=2)
    assert pc.alive(session, ser), "the device never came back after a power cut at: " + name
    ser.timeout = 5
    older = app_run["app"] + b"\xA5" * 16
    assert not pc.try_install(session, ser, "app_older", older, OLDER_VERSION), \
        "an older image was accepted after a power cut at: " + name
