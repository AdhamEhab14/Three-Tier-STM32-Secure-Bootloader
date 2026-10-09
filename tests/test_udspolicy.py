"""The diagnostic session rules and service table: the C policy against its own unit test
and against the Python model, on every combination.

The model (virtual_ecu.py) carries its own copy of the rules. If the two ever disagree the
model-based tests would be testing something the firmware does not do, so they are compared
exhaustively here.

Run it (no board needed; from the repository root):

    cd tests
    python -m pytest -v test_udspolicy.py

The C parts are compiled with a host gcc on the PATH and skipped without one.
"""
import os
import shutil
import subprocess

import pytest

from virtual_ecu import FUNCTIONAL, PHYSICAL, SESSION_CHANGES, VirtualEcu

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORE = os.path.join(ROOT, "STM32F103RBT6_Secure_Bootloader", "Core")
GCC = shutil.which("gcc")


def build(tmp_path, source, name):
    exe = str(tmp_path / name)
    subprocess.run([GCC, "-O1", "-Wall", "-Wextra", "-Werror", "-I" + os.path.join(CORE, "Inc"),
                    os.path.join(ROOT, "tests", "native", source),
                    os.path.join(CORE, "Src", "bl_udspolicy.c"), "-o", exe], check=True)
    return exe


@pytest.fixture(scope="module")
def dump(tmp_path_factory):
    if not GCC:
        pytest.skip("no C compiler")
    exe = build(tmp_path_factory.mktemp("policy"), "udspolicy_dump.c", "dump")
    return subprocess.run([exe], check=True, capture_output=True, text=True).stdout.splitlines()


def test_policy_unit_tests_pass(tmp_path):
    if not GCC:
        pytest.skip("no C compiler")
    out = subprocess.run([build(tmp_path, "udspolicy_test.c", "unit")], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout


def test_session_changes_match_the_model(dump):
    for line in dump:
        kind, *rest = line.split()
        if kind != "S":
            continue
        frm, to, allowed = (int(x) for x in rest)
        model = frm in SESSION_CHANGES and to in SESSION_CHANGES[frm]
        assert bool(allowed) == model, "session %d -> %d: C says %d, model says %s" % (frm, to, allowed, model)


def test_service_gate_matches_the_model(dump):
    checked = 0
    for line in dump:
        kind, *rest = line.split()
        if kind != "G":
            continue
        sid, session, sec, addr, nrc = (int(x) for x in rest)
        ecu = VirtualEcu()
        ecu.session = session                      # may be an invalid one: the gate must refuse it
        ecu.unlocked = sec >= 1
        model = ecu.gate(sid, functional=(addr == 2))
        assert nrc == model, ("service 0x%02X session %d security %d addr %d: C answers 0x%02X, model 0x%02X"
                              % (sid, session, sec, addr, nrc, model))
        checked += 1
    assert checked == 256 * 6 * 3 * 2
