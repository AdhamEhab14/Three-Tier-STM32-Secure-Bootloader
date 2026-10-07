"""Helpers for power-cut tests.

A power cut leaves flash in one of a small number of states. The tests get them
two ways and check the two agree:

  real cut  - the firmware runs normally and the flash model stops the machine at
              a chosen operation (renode_session.set_cut / set_cut_on_erase);
  generated - the flash image from one real cut is edited offline to describe
              every other cut point, then injected into a fresh power-up.

The generated states are only trusted because real cuts reproduce them exactly.
"""
import os
import struct

import bl_host
import sign_tool
from renode_session import ROOT

FLASH_BASE = 0x08000000
PAGE = 1024
FBL_BASE = 0x08004000
FBL_SIZE = 0x0000A000          # 40 KB, the whole FBL region
SLOT_A = 0x0800E000
SLOT_B = 0x08015000
BM_STATE = 0x0801F000
BOOT_TRIAL = 0x0801F400
CONFIG = 0x0801FC00            # application metadata, slot 0 (rollback floor)
CONFIG_ALT = 0x0801F800        # application metadata, slot 1

WORK_REL = "tests/renode/_work"
WORK = os.path.join(ROOT, "tests", "renode", "_work")


def off(addr):
    return addr - FLASH_BASE


def erased(n):
    return bytes([0xFF]) * n


def make_signed(name, payload, version, img_type):
    """Sign payload; return (path relative to the repo root, signed header bytes)."""
    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, name + ".bin")
    open(path, "wb").write(payload)
    sign_tool.sign(path, version, img_type, False)
    return "%s/%s.bin" % (WORK_REL, name), open(path + ".hdr", "rb").read()


def stage(session, ser, rel_bin, npages):
    """Erase Slot B and put the payload there (the bytes the UART path would write)."""
    import renode_session  # noqa: F401  (kept local so the module imports without Renode)
    ok, p = bl_host.transact(ser, bl_host.CMD_ERASE, struct.pack("<IB", SLOT_B, npages))
    assert bl_host.ok_reply(ok, p), "erase of the staging slot failed"
    session.load_binary(rel_bin, SLOT_B)


def run_until_cut(session, limit=30.0, step=0.01):
    """Advance until the flash model reports the cut. Fails the test if it never fires."""
    waited = 0.0
    while waited < limit:
        session.run_for(step)
        waited += step
        if session.cut_fired():
            return
    raise AssertionError("the cut never fired within %.0f s of virtual time" % limit)


def alive(session, ser):
    """True if the bootloader answers a version request after a power-up."""
    ser.timeout = 2
    ok, p = bl_host.transact(ser, bl_host.CMD_GET_VER)
    return bool(ok and list(p) == [100, 1, 5, 0])


# ---- generated states for the FBL self-update --------------------------------

def sbl_state(start, new_fbl, pages_erased, halfwords_copied=0, torn_page=None):
    """Flash as it is after the SBL has erased `pages_erased` pages of the FBL region
    and (once all 40 are erased) copied `halfwords_copied` halfwords from Slot B.

    `start` is the image taken just before the SBL's first erase. `torn_page`, if
    given as (page_index, kind), describes an erase that was interrupted on that page:
    "half" leaves the second half of the old contents, "garbage" leaves unrelated bytes.
    """
    img = bytearray(start)
    for p in range(min(pages_erased, 40)):
        a = off(FBL_BASE) + p * PAGE
        img[a:a + PAGE] = erased(PAGE)
    if pages_erased >= 40 and halfwords_copied:
        n = 2 * halfwords_copied
        a = off(FBL_BASE)
        img[a:a + n] = new_fbl[:n]
    if torn_page is not None:
        p, kind = torn_page
        a = off(FBL_BASE) + p * PAGE
        if kind == "half":
            img[a:a + PAGE // 2] = erased(PAGE // 2)
        else:
            img[a:a + PAGE] = bytes((i * 37 + 11) & 0xFF for i in range(PAGE))
    return bytes(img)


# ---- helpers for the application-install path --------------------------------

def try_install(session, ser, name, payload, version):
    """Stage `payload` as an app and send the real VERIFY. True if the firmware accepts it."""
    rel, hdr = make_signed(name, payload, version, "app")
    stage(session, ser, rel, 4)
    ok, p = bl_host.transact(ser, bl_host.CMD_VERIFY, hdr)
    return bl_host.ok_reply(ok, p)


def meta_page_rewritten(pre, final):
    """The metadata page an install rewrites: CONFIG_ALT once two records are in use, else CONFIG."""
    a = off(CONFIG_ALT)
    return CONFIG_ALT if pre[a:a + PAGE] != final[a:a + PAGE] else CONFIG


def install_state(pre, final, size, pages_erased=0, halfwords=0, meta_erased=False,
                  meta_halfwords=0, trial_erased=False, trial_programmed=False,
                  torn_page=None):
    """Flash as BL_InstallApp leaves it when power fails part-way.

    `pre` is the image before the install, `final` the image after a complete one.
    BL_InstallApp erases the app pages, copies the app, rewrites the metadata page,
    then rewrites the boot-trial page, always in that order.
    """
    img = bytearray(pre)
    npages = (size + PAGE - 1) // PAGE
    for p in range(min(pages_erased, npages)):
        a = off(SLOT_A) + p * PAGE
        img[a:a + PAGE] = erased(PAGE)
    if halfwords:
        n = 2 * halfwords
        a = off(SLOT_A)
        img[a:a + n] = final[a:a + n]
    if torn_page is not None:
        p, kind = torn_page
        a = off(SLOT_A) + p * PAGE
        if kind == "half":
            img[a:a + PAGE // 2] = erased(PAGE // 2)
        else:
            img[a:a + PAGE] = bytes((i * 53 + 7) & 0xFF for i in range(PAGE))
    if meta_erased:
        a = off(meta_page_rewritten(pre, final))
        img[a:a + PAGE] = erased(PAGE)
        n = 2 * meta_halfwords
        img[a:a + n] = final[a:a + n]
    if trial_erased:
        a = off(BOOT_TRIAL)
        img[a:a + PAGE] = erased(PAGE)
        if trial_programmed:
            img[a:a + 2] = final[a:a + 2]
    return bytes(img)
