# -*- coding: utf-8 -*-
"""
Fetch prebuilt wheels from PyPI without pip.

Why: pip needs a writable temp directory and a build-tracker directory, both of
which a locked-down sandbox can refuse (EPERM). The network itself is fine, so
this resolves the wheel URL through the PyPI JSON API, downloads it, and
unpacks it into a local directory. No compiler, no build step.

Usage:
    python tools/fetch_wheels.py <destDir> <package> [package ...]
"""
import io
import json
import os
import sys
import urllib.request
import zipfile

UA = {"User-Agent": "rippletrace-wheel-fetcher"}


def get_json(url):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=45) as r:
        return json.loads(r.read().decode("utf-8"))


def pick_wheel(pkg):
    """Choose the most specific compatible wheel for this interpreter."""
    data = get_json("https://pypi.org/pypi/%s/json" % pkg)
    version = data["info"]["version"]
    files = data["releases"][version]

    def score(name):
        n = name.lower()
        if not n.endswith(".whl"):
            return -1
        s = 0
        if "cp313" in n:
            s += 100
        elif "cp3" in n:
            s += 60
        if "win_amd64" in n:
            s += 50
        elif "win32" in n:
            s += 10
        if "abi3" in n:
            s += 20
        if "none-any" in n:
            s += 5
        return s

    best, best_score = None, -1
    for f in files:
        sc = score(f["filename"])
        if sc > best_score:
            best, best_score = f, sc
    return version, best


def unpack(url, dest):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=120) as r:
        blob = r.read()
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        z.extractall(dest)
    return len(blob)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    dest = sys.argv[1]
    os.makedirs(dest, exist_ok=True)

    failures = []
    for pkg in sys.argv[2:]:
        try:
            version, wheel = pick_wheel(pkg)
            if wheel is None:
                print("  FAIL  %-22s no wheel published" % pkg)
                failures.append(pkg)
                continue
            size = unpack(wheel["url"], dest)
            print("  OK    %-22s %s  %s  (%.1f KB)"
                  % (pkg, version, wheel["filename"], size / 1024.0))
        except Exception as e:
            print("  FAIL  %-22s %s: %s" % (pkg, type(e).__name__, str(e)[:100]))
            failures.append(pkg)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
