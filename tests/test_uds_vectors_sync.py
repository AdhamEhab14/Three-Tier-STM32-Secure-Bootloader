"""The C table of shared UDS requests must match the text file it is made from.

Run it (no board needed; from the repository root):

    cd tests
    python -m pytest -v test_uds_vectors_sync.py
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import gen_uds_vectors


def test_the_committed_header_matches_the_vector_file():
    subprocess.check_call([sys.executable, os.path.join(ROOT, "scripts", "gen_uds_vectors.py"), "--check"])


def test_every_vector_fits_the_table():
    vectors = gen_uds_vectors.parse()
    assert len(vectors) > 20
    for req, resp, note in vectors:
        assert 1 <= len(req) <= gen_uds_vectors.MAX_REQ, note
        assert 1 <= len(resp) <= gen_uds_vectors.MAX_RESP, note
