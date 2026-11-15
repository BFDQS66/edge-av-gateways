#!/usr/bin/env python3
"""Local ZLMediaKit publish hook for the approved stream.

Only the board-local packet gate may publish. Keep this listener on loopback;
ZLMediaKit supplies the peer IP in its on_publish event.
"""

import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
import hmac
import json
from pathlib import Path


def decision(event: object, publisher_ip: str, token: str) -> dict:
    allowed = (
        isinstance(event, dict)
        and event.get("ip") == publisher_ip
        and event.get("app") == "approved"
        and event.get("stream") == "obs"
        and event.get("schema") == "rtmp"
        and event.get("protocol") == "rtmp"
        and hmac.compare_digest(str(event.get("params", "")), "token=" + token)
    )
    return {"code": 0 if allowed else -1,
            "msg": "approved publisher" if allowed else "publisher denied"}


def serve(publisher_ip: str, port: int, token: str) -> None:
    class Handler(BaseHTTPRequestHandler):
        def do_POST(self) -> None:
            self.connection.settimeout(2)
            if self.path != "/on_publish":
                self.send_error(404)
                return
            try:
                length = int(self.headers.get("Content-Length", "-1"))
                if length < 0 or length > 8192:
                    raise ValueError("invalid body size")
                event = json.loads(self.rfile.read(length))
                result = decision(event, publisher_ip, token)
            except (ValueError, UnicodeError, json.JSONDecodeError):
                result = {"code": -1, "msg": "invalid hook request"}
            body = json.dumps(result, separators=(",", ":")).encode("ascii")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *args: object) -> None:
            pass  # Never log publisher URLs or media metadata on the board.

    server = HTTPServer(("127.0.0.1", port), Handler)
    server.serve_forever(poll_interval=0.2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--publisher-ip", required=True)
    parser.add_argument("--token-file", type=Path, required=True)
    parser.add_argument("--port", type=int, default=18993)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error("port must be 1024..65535")
    token = args.token_file.read_text(encoding="ascii").strip()
    if len(token) < 32 or not all(c.isalnum() or c in "-_" for c in token):
        parser.error("token file must contain a generated URL-safe token")
    serve(args.publisher_ip, args.port, token)
