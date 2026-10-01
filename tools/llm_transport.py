# -*- coding: utf-8 -*-
"""
TLS transport helper for 涟漪 RippleTrace.

Why this exists
---------------
涟漪 RippleTrace's native path uses WinHTTP. On some locked-down Windows environments the
native TLS stack is unusable - Schannel fails with SEC_E_NO_CREDENTIALS, which
surfaces in WinHTTP as 12185 (ERROR_WINHTTP_UNRECOGNIZED_SCHEME) and in curl as
"schannel: AcquireCredentialsHandle failed". A Python runtime ships its own
OpenSSL, so it can still complete the handshake.

Contract
--------
Reads one JSON object from stdin:
    {"url": "...", "apiKey": "...", "payload": {...}, "timeoutSec": 60}
Writes one JSON object to stdout:
    {"ok": true,  "content": "<assistant message>"}
    {"ok": false, "error": "<reason>"}

The API key is passed via stdin, never as a command line argument, so it does
not appear in the process list.

Encoding
--------
stdin and stdout are used as BYTES, via sys.stdin.buffer / sys.stdout.buffer.

They must not go through the sys.stdin / sys.stdout text wrappers. On Windows
those decode with the locale code page and, crucially, with
errors="surrogateescape". 涟漪 RippleTrace writes valid UTF-8, so on a Chinese install
every Chinese character was decoded as cp936; the byte sequences that are not
valid cp936 became lone surrogates (U+DC80..U+DCFF); json.dumps then escaped
them as \\udcXX; and the endpoint rejected the whole request:

    HTTP 400 ... Failed to parse the request body as JSON:
               messages[1].content: lone leading surrogate in hex escape

Reading and writing bytes takes the locale out of the picture entirely, so the
payload reaches the endpoint exactly as 涟漪 RippleTrace produced it. tools/test_helper.py
pins this behaviour.
"""
import json
import sys
import urllib.error
import urllib.request

# Belt and braces: anything that still reaches a text wrapper (a traceback, for
# instance) should be UTF-8 rather than mojibake. Binary I/O below does not rely
# on this.
for _stream in (sys.stdin, sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except Exception:
        pass


def write_out(obj):
    """Write the response as UTF-8 bytes, independent of the locale code page."""
    sys.stdout.buffer.write(json.dumps(obj, ensure_ascii=False).encode("utf-8"))
    sys.stdout.buffer.flush()


def fail(msg):
    write_out({"ok": False, "error": str(msg)})
    return 0


def main():
    raw_bytes = sys.stdin.buffer.read()
    try:
        raw = raw_bytes.decode("utf-8")
    except UnicodeDecodeError as e:
        return fail("helper received bytes that are not valid UTF-8: %s" % e)

    try:
        req = json.loads(raw)
    except Exception as e:
        return fail("helper received invalid JSON on stdin: %s" % e)

    url = req.get("url") or ""
    api_key = req.get("apiKey") or ""
    payload = req.get("payload")
    timeout = int(req.get("timeoutSec") or 60)

    if not url:
        return fail("helper received no url")
    if payload is None:
        return fail("helper received no payload")

    # ensure_ascii keeps the wire body pure ASCII, so there is no charset
    # ambiguity in the HTTP request itself. Any lone surrogate surviving this
    # far would surface as \udXXX here, which is exactly what we are preventing.
    body = json.dumps(payload).encode("utf-8")
    http_req = urllib.request.Request(url, data=body, method="POST")
    http_req.add_header("Content-Type", "application/json")
    if api_key:
        http_req.add_header("Authorization", "Bearer " + api_key)

    try:
        with urllib.request.urlopen(http_req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:400]
        return fail("HTTP %s from the model endpoint: %s" % (e.code, detail))
    except Exception as e:
        return fail("transport error: %s: %s" % (type(e).__name__, e))

    try:
        data = json.loads(text)
    except Exception as e:
        return fail("model endpoint returned non-JSON: %s" % e)

    if isinstance(data, dict) and data.get("error"):
        err = data["error"]
        msg = err.get("message") if isinstance(err, dict) else str(err)
        return fail("API error: %s" % msg)

    content = None
    try:
        content = data["choices"][0]["message"]["content"]
    except Exception:
        pass

    if not content:
        return fail("model response contained no message content")

    write_out({"ok": True, "content": content})
    return 0


if __name__ == "__main__":
    sys.exit(main())
