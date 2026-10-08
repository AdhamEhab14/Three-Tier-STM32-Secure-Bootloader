#!/usr/bin/env python3
"""Turn tests/vectors/uds_common.txt into the C table the on-chip self-test replays.

    python scripts/gen_uds_vectors.py            write the header
    python scripts/gen_uds_vectors.py --check    fail if the committed header is out of date
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "tests", "vectors", "uds_common.txt")
OUT = os.path.join(ROOT, "STM32F103RBT6_Secure_Bootloader", "Core", "Inc", "bl_uds_vectors.h")

MAX_REQ, MAX_RESP = 12, 6


def parse(path=SRC):
    """[(request bytes, response prefix bytes, comment)]"""
    vectors = []
    for raw in open(path, encoding="utf8"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        req, resp, note = (part.strip() for part in line.split("|", 2))
        vectors.append((bytes.fromhex(req), bytes.fromhex(resp), note))
    return vectors


def render(vectors):
    rows = []
    for req, resp, note in vectors:
        assert len(req) <= MAX_REQ and len(resp) <= MAX_RESP, note
        r = ", ".join("0x%02X" % b for b in req)
        e = ", ".join("0x%02X" % b for b in resp)
        rows.append("    { %d, { %s }, %d, { %s } },   /* %s */" % (len(req), r, len(resp), e, note))
    return """/* Generated from tests/vectors/uds_common.txt by scripts/gen_uds_vectors.py. Do not edit.
   Requests both UDS servers must answer alike; the iso14229 self-test replays them. */
#ifndef BL_UDS_VECTORS_H
#define BL_UDS_VECTORS_H

#include <stdint.h>

#define BL_UDS_VEC_MAX_REQ   %d
#define BL_UDS_VEC_MAX_RESP  %d

typedef struct {
    uint8_t req_len;
    uint8_t req[BL_UDS_VEC_MAX_REQ];
    uint8_t exp_len;
    uint8_t exp[BL_UDS_VEC_MAX_RESP];
} bl_uds_vec_t;

static const bl_uds_vec_t bl_uds_vectors[] = {
%s
};

#define BL_UDS_VEC_COUNT  (sizeof(bl_uds_vectors) / sizeof(bl_uds_vectors[0]))

#endif /* BL_UDS_VECTORS_H */
""" % (MAX_REQ, MAX_RESP, "\n".join(rows))


if __name__ == "__main__":
    text = render(parse())
    if "--check" in sys.argv:
        current = open(OUT, encoding="utf8", newline="").read().replace("\r\n", "\n") if os.path.exists(OUT) else ""
        if current != text:
            sys.exit("bl_uds_vectors.h is out of date: run scripts/gen_uds_vectors.py")
    else:
        open(OUT, "w", encoding="utf8", newline="\n").write(text)
