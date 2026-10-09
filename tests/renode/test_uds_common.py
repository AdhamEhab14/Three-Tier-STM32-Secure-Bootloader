"""The requests both UDS servers must answer alike, on the production command layer.

tests/vectors/uds_common.txt is the shared list. The iso14229 server replays the same list in
its on-chip self-test (test_uds_selftest.py), so a difference between the two servers shows up
as a failure on one side or the other.

Run it (no board needed; needs Renode and the build-test firmware, see docs/testing.md
section 6 for the one-time build):

    cd tests/renode
    python -m pytest -v test_uds_common.py
"""
import os

import pytest

import bl_host
import uds_helpers as uh
from renode_session import RenodeSerial, ROOT

import sys
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import gen_uds_vectors


@pytest.fixture()
def board(session):
    session.power_cycle(settle=0.0)
    ser = RenodeSerial(session, timeout=5)
    uh.wait_for_fbl(session, ser)
    session.idle(1.5)            # past the SecurityAccess boot delay, still in the default session
    return ser


def test_the_production_server_answers_the_shared_requests(board):
    for number, (req, want, note) in enumerate(gen_uds_vectors.parse(), start=1):
        ok, resp = bl_host.transact(board, bl_host.CMD_UDS, req)
        assert ok, "vector %d (%s): no reply" % (number, note)
        assert bytes(resp[:len(want)]) == want, "vector %d (%s): sent %s, expected %s, got %s" % (
            number, note, req.hex(" "), want.hex(" "), bytes(resp).hex(" "))
