"""Fixtures that boot the real firmware (Boot Manager + FBL) in Renode.

Needs Renode (RENODE_PATH or on PATH) and a build made with
  cmake -S . -B build-test -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DBL_TEST_KEYS=ON
(or set BL_BUILD_DIR). Tests skip cleanly when either is missing.
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "host"))

from renode_session import RenodeSession, RenodeSerial, find_renode  # noqa: E402

BUILD = os.environ.get("BL_BUILD_DIR", "build-test")
KEYS = os.path.join(ROOT, "tests", "keys")
os.environ.setdefault("BL_KEYS_DIR", KEYS)

# The TEST keys are derived from fixed seeds. The .bin files are git-ignored, so
# regenerate them when a fresh checkout does not have them yet.
if not os.path.exists(os.path.join(KEYS, "bl_private.bin")):
    import subprocess
    subprocess.check_call([sys.executable, os.path.join(KEYS, "make_test_keys.py")])


@pytest.fixture(scope="module")
def session():
    exe = find_renode()
    if not exe:
        pytest.skip("Renode not found (set RENODE_PATH)")
    if not os.path.exists(os.path.join(ROOT, BUILD, "fbl.elf")):
        pytest.skip("firmware not built in %s" % BUILD)
    s = RenodeSession(exe, build_dir=BUILD.replace("\\", "/"))
    s.run_for(1.5)                      # Boot Manager (0.6 s LED delay) then FBL start-up
    yield s
    s.close()


@pytest.fixture()
def ser(session):
    session.rx.clear()
    return RenodeSerial(session)


@pytest.fixture(scope="module")
def build_dir():
    return os.path.join(ROOT, BUILD)
