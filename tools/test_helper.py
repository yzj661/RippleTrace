# -*- coding: utf-8 -*-
"""
Regression test for the TLS helper's stdio encoding.

The bug this pins down
----------------------
tools/llm_transport.py used to read sys.stdin and write sys.stdout - the *text*
wrappers. On Windows those decode with the locale code page and, crucially,
with errors="surrogateescape". 涟漪 RippleTrace writes valid UTF-8, so on a Chinese
install each Chinese character was decoded as cp936; byte sequences that are
not valid cp936 became lone surrogates (U+DC80..U+DCFF); json.dumps escaped
them as \\udcXX; and the model endpoint rejected the whole request:

    HTTP 400 ... Failed to parse the request body as JSON:
               messages[1].content: lone leading surrogate in hex escape

The observation that made this hard to find: the failure only appeared on
repos containing non-ASCII text, and the byte column in the error was always
1451 - the same bytes, re-encoded.

What is checked here
--------------------
  * a payload containing Chinese arrives at the endpoint byte-for-byte
  * the wire body contains no surrogate escape and is valid UTF-8
  * non-ASCII text in the model's *reply* survives the return trip
  * stdin that is not valid UTF-8 is reported, not crashed on

Runs offline: it starts its own local endpoint on an ephemeral port.
"""
import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(HERE, "llm_transport.py")

CHINESE = "俄罗斯方块1 中文注释：光标移动 设置控制台颜色"
REPLY = "模型回复：需要检查 gotoXY 的调用方"

RECEIVED_BODIES = []
REPLY_TEXT = [REPLY]


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        RECEIVED_BODIES.append(self.rfile.read(n))
        payload = {"choices": [{"message": {"role": "assistant",
                                            "content": REPLY_TEXT[0]}}]}
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def run_helper(request_bytes):
    return subprocess.run(
        [sys.executable, HELPER],
        input=request_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=os.path.dirname(HERE),
    )


def main():
    server = HTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()

    checks = 0
    failures = 0

    def check(cond, what):
        nonlocal checks, failures
        checks += 1
        if not cond:
            failures += 1
            print("  FAIL  %s" % what)

    payload = {
        "model": "test-model",
        "temperature": 0.2,
        "messages": [
            {"role": "system", "content": "static analysis decides what to check"},
            {"role": "user", "content": CHINESE},
        ],
    }
    request = {
        "url": "http://127.0.0.1:%d/v1/chat/completions" % port,
        "apiKey": "sk-not-a-real-key",
        "payload": payload,
        "timeoutSec": 20,
    }

    # ---------------------------------------------------------------
    #  1. Non-ASCII payload survives the trip to the endpoint.
    # ---------------------------------------------------------------
    print("[ helper stdio encoding ]")
    proc = run_helper(json.dumps(request, ensure_ascii=False).encode("utf-8"))
    check(proc.returncode == 0,
          "helper exits 0 (stderr: %s)" % proc.stderr.decode("utf-8", "replace")[:300])
    check(len(RECEIVED_BODIES) == 1, "endpoint received exactly one request")

    if RECEIVED_BODIES:
        wire = RECEIVED_BODIES[0]
        wire_text = wire.decode("utf-8", "replace")

        check(wire_text.count("\\ud") == 0 and wire_text.count("\\uD") == 0,
              "wire body contains no surrogate escape")
        try:
            wire.decode("utf-8")
            check(True, "wire body is valid UTF-8")
        except UnicodeDecodeError as e:
            check(False, "wire body is valid UTF-8 (%s)" % e)

        sent = json.loads(wire_text)
        check(sent["messages"][1]["content"] == CHINESE,
              "user message arrives unchanged: expected %r got %r"
              % (CHINESE, sent["messages"][1].get("content")))
        check(sent["messages"][0]["content"] == payload["messages"][0]["content"],
              "system message arrives unchanged")

    # ---------------------------------------------------------------
    #  2. The reply comes back as real UTF-8, not mojibake.
    # ---------------------------------------------------------------
    out = json.loads(proc.stdout.decode("utf-8", "replace"))
    check(out.get("ok") is True, "helper reports ok=true (error: %r)" % out.get("error"))
    check(out.get("content") == REPLY, "Chinese in the model reply survives the return trip")

    # ---------------------------------------------------------------
    #  3. Bad input is reported, not crashed on.
    # ---------------------------------------------------------------
    REPLY_TEXT[0] = REPLY
    bad = run_helper(b'{"url":"http://127.0.0.1:%d/x",' % port + b'\xff\xfe"payload":{}}')
    bad_out = bad.stdout.decode("utf-8", "replace")
    try:
        bad_obj = json.loads(bad_out)
        check(bad_obj.get("ok") is False, "invalid UTF-8 on stdin reports ok=false")
        check("not valid UTF-8" in (bad_obj.get("error") or ""),
              "invalid UTF-8 on stdin reports a UTF-8 error, got %r"
              % bad_obj.get("error"))
    except Exception as e:
        check(False, "invalid input still produced parseable output (%s): %r" % (e, bad_out[:200]))

    print()
    print("----------------------------------------")
    print("checks: %d   failures: %d" % (checks, failures))
    print("ALL TESTS PASSED" if failures == 0 else "TESTS FAILED")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
