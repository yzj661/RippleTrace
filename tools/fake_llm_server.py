# -*- coding: utf-8 -*-
"""
A fake OpenAI-compatible endpoint used to test the AI layer end to end,
over real HTTP, with no API key and no internet.

The reply it returns is deliberately adversarial:
  * one point for a function that IS in the impact set   -> must survive
  * one point for a function that EXISTS in the repo but is NOT impacted
                                                          -> must be rejected
  * one point for a function that does not exist at all   -> must be rejected
  * one point whose id is given without the namespace prefix -> must resolve

If the pipeline shows the middle two, the grounding guarantee is broken.
"""
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

REPLY = {
    "summary": "The change sits on a widely used crypto primitive; impact crosses "
               "the audit layer into transfer tests.",
    "points": [
        {"function": "test_sha256_abc", "priority": 1,
         "what_to_check": "the known-answer vectors still match after the edit",
         "why_it_matters": "it calls the changed function directly, so it fails first"},
        {"function": "trim", "priority": 1,
         "what_to_check": "this function was NOT reported as impacted",
         "why_it_matters": "the model should not be allowed to say this"},
        {"function": "totally_made_up_function", "priority": 2,
         "what_to_check": "this function does not exist anywhere",
         "why_it_matters": "the model should not be allowed to say this"},
        {"function": "AuditLog::record", "priority": 2,
         "what_to_check": "the audit fingerprint recorded for each entry",
         "why_it_matters": "it reaches the change through audit_fingerprint"},
    ],
}

CAPTURED = {"body": None, "auth": None}


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode("utf-8", "replace")
        CAPTURED["body"] = raw
        CAPTURED["auth"] = self.headers.get("Authorization")
        try:
            parsed = json.loads(raw)
            with open("_fake_request.json", "w", encoding="utf-8") as f:
                json.dump(parsed, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

        payload = {
            "id": "fake-1",
            "object": "chat.completion",
            "model": "fake-model",
            "choices": [{
                "index": 0,
                "message": {"role": "assistant",
                            "content": json.dumps(REPLY, ensure_ascii=False)},
                "finish_reason": "stop",
            }],
        }
        body = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass          # keep the test output clean


if __name__ == "__main__":
    server = HTTPServer(("127.0.0.1", 8077), Handler)
    server.serve_forever()
