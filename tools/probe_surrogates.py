"""Find byte sequences that a naive UTF-8 range check accepts but strict UTF-8
rejects - most importantly WTF-8 encoded surrogates. Those are exactly the bytes
that make a model endpoint answer HTTP 400 "lone leading surrogate in hex
escape", so this is the first thing to run when that happens.

Usage:  python tools/probe_surrogates.py [path]      (default: current directory)
"""
import os, sys, re

# Chinese paths and file names must survive the console encoding.
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except Exception:
        pass

root = sys.argv[1] if len(sys.argv) > 1 else "."
if not os.path.isdir(root):
    print(f"not a directory: {root}")
    raise SystemExit(2)
skip = {".git","build","out",".vs","x64","Debug","Release","node_modules","__pycache__","target"}
bad = []
surrogate = []
for dp, dns, fns in os.walk(root):
    dns[:] = [d for d in dns if d not in skip]
    for fn in fns:
        p = os.path.join(dp, fn)
        try:
            b = open(p, "rb").read()
        except Exception:
            continue
        # Python strict utf-8 REJECTS encoded surrogates; C++ byte-range check does not.
        try:
            b.decode("utf-8")
            naive_ok = True
            strict_ok = True
        except UnicodeDecodeError:
            strict_ok = False
            naive_ok = None
        # naive byte-range check, mirroring the C++ validator
        i, n = 0, len(b)
        valid = True
        while i < n:
            c = b[i]
            if c < 0x80: i += 1; continue
            if 0xC2 <= c <= 0xDF: L = 2
            elif 0xE0 <= c <= 0xEF: L = 3
            elif 0xF0 <= c <= 0xF4: L = 4
            else: valid = False; break
            if i + L > n: valid = False; break
            if not all(0x80 <= x <= 0xBF for x in b[i+1:i+L]): valid = False; break
            i += L
        if not valid:
            bad.append((p, len(b), "naive-check-rejects"))
        elif not strict_ok:
            bad.append((p, len(b), "PASSES naive check but Python strict rejects"))
        for m in re.finditer(rb"\xed[\xa0-\xbf][\x80-\xbf]", b):
            surrogate.append((p, m.start()))
print(f"files examined under {root}")
print(f"problem files: {len(bad)}")
for p, n, why in bad[:40]:
    print(f"  [{why}]  {n} bytes  {p}")
print(f"WTF-8 surrogate sequences found: {len(surrogate)}")
for p, off in surrogate[:20]:
    print(f"  offset {off}  {p}")
