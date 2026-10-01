# -*- coding: utf-8 -*-
"""
Measure the encoding hazard in a real source tree.

The failure this diagnoses:
    HTTP 400 ... lone leading surrogate in hex escape

That happens when a source file is NOT UTF-8 (for example a GBK-encoded
Chinese comment) but is handed to the analysis pipeline as if it were. GBK byte
pairs frequently look like well-formed UTF-8 sequences, so a "is this valid
UTF-8?" check passes and the corruption travels all the way to the JSON encoder
as an unpaired surrogate.

Usage: python tools/probe_encoding.py <dir>
"""
import os
import sys

SKIP = {".git", "build", "out", ".vs", "x64", "Debug", "Release",
        "__pycache__", "node_modules", "target", "_vendor"}
EXTS = {".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx", ".h", ".inl", ".ipp",
        ".java", ".py"}


def classify(raw):
    """Return (kind, detail) for a byte string."""
    try:
        raw.decode("utf-8")
        return "utf8", "valid UTF-8"
    except UnicodeDecodeError as e:
        first_bad = e.start
        try:
            text = raw.decode("gbk")
            return "gbk", "invalid UTF-8 at byte %d, but decodes as GBK" % first_bad
        except Exception:
            return "unknown", "invalid UTF-8 at byte %d" % first_bad


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = sys.argv[1]

    counts = {"utf8": 0, "gbk": 0, "unknown": 0}
    bad = []

    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if d not in SKIP]
        for f in fn:
            if os.path.splitext(f)[1].lower() not in EXTS:
                continue
            p = os.path.join(dp, f)
            try:
                raw = open(p, "rb").read()
            except OSError:
                continue
            kind, detail = classify(raw)
            counts[kind] += 1
            if kind != "utf8":
                bad.append((os.path.relpath(p, root), kind, detail, len(raw)))

    print("=" * 74)
    print(" source encoding audit: %s" % root)
    print("=" * 74)
    print("  valid UTF-8 : %d file(s)" % counts["utf8"])
    print("  GBK         : %d file(s)   <-- these corrupt the analysis" % counts["gbk"])
    print("  undecodable : %d file(s)" % counts["unknown"])
    if bad:
        print()
        print("  files that are NOT UTF-8:")
        for rel, kind, detail, size in sorted(bad):
            print("    [%-7s] %-46s %6d bytes" % (kind, rel, size))
        print()
        print("  Impact: any comment or string in these files reaches the model")
        print("  prompt as mojibake, and a GBK byte pair that happens to look like")
        print("  a UTF-8 surrogate makes the JSON body invalid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
