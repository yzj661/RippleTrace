# -*- coding: utf-8 -*-
"""Probe whether the API is reachable from this machine at all.

This separates two very different diagnoses:
  * the key/network is fine and only WinHTTP is misbehaving, or
  * the environment cannot reach the endpoint at all.
"""
import json
import os
import ssl
import sys
import urllib.request

KEY = os.environ.get("PROBE_KEY", "")
URL = "https://api.deepseek.com/v1/chat/completions"

print("python:", sys.version.split()[0])
print("openssl:", ssl.OPENSSL_VERSION)
print("url:", URL)
print()

body = json.dumps({
    "model": "deepseek-chat",
    "messages": [{"role": "user", "content": "Reply with the single word: ok"}],
    "max_tokens": 5,
}).encode("utf-8")

req = urllib.request.Request(URL, data=body, method="POST")
req.add_header("Content-Type", "application/json")
req.add_header("Authorization", "Bearer " + KEY)

try:
    with urllib.request.urlopen(req, timeout=45) as r:
        raw = r.read().decode("utf-8", "replace")
        print("HTTP", r.status)
        data = json.loads(raw)
        msg = data.get("choices", [{}])[0].get("message", {}).get("content")
        print("model reply:", repr(msg))
        print("usage:", data.get("usage"))
        print()
        print("RESULT: the key and the network both work.")
except urllib.error.HTTPError as e:
    detail = e.read().decode("utf-8", "replace")[:400]
    print("HTTP", e.code, detail)
    print()
    print("RESULT: reached the server but it rejected the request "
          "(401/402 would mean a key or balance problem).")
except Exception as e:
    print("FAILED:", type(e).__name__, e)
    print()
    print("RESULT: could not reach the endpoint at all from this environment.")
