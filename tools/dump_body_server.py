"""Echo endpoint that records the exact request body 涟漪 RippleTrace sends.

Used to locate byte-level corruption in the model prompt. It writes the raw
body to dump_body.bin and answers with a well-formed OpenAI-shaped response,
so the client sees a successful call.
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

# Written next to the current directory, not to a fixed absolute path - this
# script ships with the project and has to work from wherever it is unpacked.
OUT = os.path.join(os.getcwd(), "dump_body.bin")


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n)
        with open(OUT, "wb") as f:
            f.write(raw)

        # Report what we can decode, so the terminal shows the verdict too.
        try:
            raw.decode("utf-8")
            verdict = "python strict decode: OK"
        except UnicodeDecodeError as e:
            verdict = f"python strict decode: FAIL at {e.start} ({e.reason})"

        text = raw.decode("utf-8", "replace")
        lone = []
        i = 0
        while True:
            i = text.find("\\u", i)
            if i < 0:
                break
            try:
                cp = int(text[i + 2:i + 6], 16)
            except ValueError:
                i += 2
                continue
            if 0xD800 <= cp <= 0xDFFF:
                lone.append((i, text[i:i + 6]))
            i += 2

        sys.stderr.write(f"[dump] body={len(raw)} bytes  {verdict}  "
                         f"surrogate-escapes={len(lone)}\n")
        for off, esc in lone[:5]:
            sys.stderr.write(f"       escape {esc} at offset {off}\n")
            sys.stderr.write(f"       context: {text[max(0,off-80):off+20]!r}\n")
        sys.stderr.flush()

        payload = {
            "choices": [{
                "message": {"role": "assistant", "content": json.dumps(
                    {"summary": "dump endpoint", "points": []})}
            }]
        }
        body = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8077
    sys.stderr.write(f"[dump] listening on 127.0.0.1:{port}\n")
    sys.stderr.flush()
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
