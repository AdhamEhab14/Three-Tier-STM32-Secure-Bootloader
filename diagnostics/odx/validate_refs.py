#!/usr/bin/env python3
"""
validate_refs.py - a quick structural sanity check for the ODX file, before
handing it to a full ODX tool.

odxtools does the real schema validation, but it needs installing and it stops
at the first thing it dislikes. This does the two checks that actually catch
hand-authoring slips - every ID is unique, and every ID-REF points at an ID
that exists - and reports *all* problems at once, with no dependencies beyond
the standard library.

    python validate_refs.py SecureBootloader.odx-d
"""
import sys
import xml.etree.ElementTree as ET
from collections import Counter


def check(path):
    # A malformed document fails here with a clear line/column, which is
    # already more useful than a schema error buried three levels down.
    root = ET.parse(path).getroot()

    # ODXLINK works on plain 'ID' / 'ID-REF' attributes, no namespaces, so a
    # flat walk over every element is enough.
    ids = [el.attrib["ID"] for el in root.iter() if "ID" in el.attrib]
    refs = [(el.tag, el.attrib["ID-REF"]) for el in root.iter() if "ID-REF" in el.attrib]

    problems = []

    duplicates = [i for i, n in Counter(ids).items() if n > 1]
    for dup in duplicates:
        problems.append(f"duplicate ID: {dup}")

    known = set(ids)
    for tag, ref in refs:
        if ref not in known:
            problems.append(f"dangling ID-REF on <{tag}>: {ref}")

    print(f"{path}")
    print(f"  {len(ids)} IDs, {len(refs)} references")
    if problems:
        print(f"  {len(problems)} problem(s):")
        for p in problems:
            print(f"    - {p}")
        return 1

    print("  OK - all references resolve, no duplicate IDs")
    return 0


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "SecureBootloader.odx-d"
    sys.exit(check(target))
