#!/usr/bin/env python3
"""LAN HTTP POST receiver for ESP32 transport diagnostic.

Accepts POST requests of any size (up to 200KB).
Outputs only: timestamp, Content-Length, received body length, SHA256.
Never prints the body content.

Usage:
    python tools/large_post_receiver.py [--port 8089]

Then flash ESP32 with VOICE_CLOUD_DIAGNOSTIC=ON and
configure VOICE_CLOUD_DIAG_LAN_URL to http://<this-machine-ip>:8089/post
"""

import argparse
import hashlib
import json
import sys
import threading
from datetime import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler

MAX_BODY = 200 * 1024  # 200KB


class PostReceiver(BaseHTTPRequestHandler):
    """Minimal POST receiver — logs length + SHA256, returns JSON ack."""

    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", 0))
        if content_length > MAX_BODY:
            self.send_response(413)
            self.end_headers()
            self.wfile.write(b"too large")
            return

        body = b""
        remaining = content_length
        while remaining > 0:
            chunk = self.rfile.read(min(remaining, 8192))
            if not chunk:
                break
            body += chunk
            remaining -= len(chunk)

        digest = hashlib.sha256(body).hexdigest()
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(
            f"[{ts}] POST {self.path}  "
            f"Content-Length={content_length}  "
            f"received={len(body)}  "
            f"SHA256={digest[:16]}...",
            flush=True,
        )

        resp = json.dumps({
            "status": "ok",
            "received": len(body),
            "sha256": digest,
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def do_GET(self):
        """Health check endpoint."""
        resp = b'{"status":"ready"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, fmt, *args):
        """Suppress default access log — we print our own."""
        pass


def get_local_ip():
    """Best-effort LAN IP detection."""
    import socket
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def main():
    parser = argparse.ArgumentParser(description="LAN HTTP POST receiver")
    parser.add_argument("--port", type=int, default=8089, help="Listen port")
    args = parser.parse_args()

    ip = get_local_ip()
    server = HTTPServer(("0.0.0.0", args.port), PostReceiver)
    print(f"LAN POST receiver listening on http://{ip}:{args.port}/post")
    print(f"Configure ESP32: VOICE_CLOUD_DIAG_LAN_URL=\"http://{ip}:{args.port}/post\"")
    print("Press Ctrl+C to stop.\n", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.shutdown()


if __name__ == "__main__":
    main()
