# -*- coding: utf-8 -*-
"""Check which package sources are reachable from this machine.

Python's TLS stack works here (it bundles OpenSSL) while the native Windows
stack does not, so this probe is the reliable one.
"""
import json
import ssl
import sys
import urllib.error
import urllib.request

TARGETS = [
    ("PyPI simple index",      "https://pypi.org/simple/"),
    ("PyPI JSON API",          "https://pypi.org/pypi/tree-sitter/json"),
    ("npm registry",           "https://registry.npmjs.org/tree-sitter"),
    ("GitHub",                 "https://github.com/"),
    ("GitHub codeload (tarball)", "https://codeload.github.com/tree-sitter/tree-sitter/legacy.tar.gz/refs/heads/master"),
    ("raw.githubusercontent",  "https://raw.githubusercontent.com/tree-sitter/tree-sitter/master/README.md"),
    ("DeepSeek API",           "https://api.deepseek.com/"),
]

print("python:", sys.version.split()[0], "| openssl:", ssl.OPENSSL_VERSION)
print()

for name, url in TARGETS:
    try:
        req = urllib.request.Request(url, method="GET")
        req.add_header("User-Agent", "rippletrace-probe")
        with urllib.request.urlopen(req, timeout=20) as r:
            data = r.read(2048)
            print("  OK    %-28s HTTP %s  (%d bytes read)" % (name, r.status, len(data)))
    except urllib.error.HTTPError as e:
        print("  OK    %-28s HTTP %s (reachable, server said %s)" % (name, e.code, e.code))
    except Exception as e:
        print("  FAIL  %-28s %s: %s" % (name, type(e).__name__, str(e)[:90]))
