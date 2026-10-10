"""The real C firmware, running in the emulator, driven by the real host tool.

Run it (no board needed; needs Renode and the build-test firmware, see docs/testing.md
section 6 for the one-time build):

    cd tests/renode
    python -m pytest -v test_boot.py
"""
import bl_host


def test_fbl_answers_version(ser):
    ok, p = bl_host.transact(ser, bl_host.CMD_GET_VER)
    assert ok
    assert list(p) == [100, 2, 0, 0]          # vendor 100, v2.0.0


def test_power_on_self_test_passes(ser):
    ok, p = bl_host.transact(ser, bl_host.CMD_BIST)
    assert ok, "BIST command not acknowledged"
    assert len(p) >= 1
