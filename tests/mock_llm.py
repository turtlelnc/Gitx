#!/usr/bin/env python3
"""Minimal OpenAI-compatible chat completions mock server for testing gitx AI.

Usage: python3 tests/mock_llm.py <port>
Replies to any /chat/completions request with a fixed assistant message that
passes gitx's commit validation, so tests can assert end-to-end flow without
hitting a real LLM.
"""
import http.server
import json
import re
import sys


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length) or b"{}")
        messages = body.get("messages", [])
        user = next((m["content"] for m in reversed(messages) if m["role"] == "user"), "")
        # Extract a spec-compliant commit message from the diff if present,
        # else return a fixed valid one.
        match = re.search(r"(feat|fix|docs|refactor|test|chore)\([^)]*\): .+", user)
        reply = match.group(0) if match else "feat(ai): mock reply"
        payload = json.dumps({"choices": [{"message": {"role": "assistant", "content": reply}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18999
    http.server.HTTPServer(("127.0.0.1", port), Handler).serve_forever()
