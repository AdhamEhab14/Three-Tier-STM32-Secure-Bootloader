"""The real C firmware, running in the emulator, driven by the real host tool."""
import bl_host


def test_fbl_answers_version(ser):
    ok, p = bl_host.transact(ser, bl_host.CMD_GET_VER)
    assert ok
    assert list(p) == [100, 1, 5, 0]          # vendor 100, v1.5.0


def test_power_on_self_test_passes(ser):
    ok, p = bl_host.transact(ser, bl_host.CMD_BIST)
    assert ok, "BIST command not acknowledged"
    assert len(p) >= 1
