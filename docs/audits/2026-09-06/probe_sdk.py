"""Bounded loopback-only audit observations; exit 0 is not a security verdict."""

import collections
import json
import os
from pathlib import Path
import socket
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "sdk/python/src"))
from maelys_egress import Destination, EgressConfig, EgressProcess


def queue_probe(config, binary):
    counts = collections.Counter()
    receipts_done = threading.Event()

    def callback(event):
        counts[event["event"]] += 1
        if counts["receipt"] == 128:
            receipts_done.set()

    with EgressProcess(config, binary=binary, on_event=callback) as egress:
        for _ in range(128):
            with socket.create_connection(("127.0.0.1", egress.proxy_port), timeout=2) as client:
                client.sendall(
                    b"CONNECT blocked.invalid:443 HTTP/1.1\r\n"
                    b"Host: blocked.invalid:443\r\n\r\n"
                )
                deadline = time.monotonic() + 2
                while client.recv(4096):
                    if time.monotonic() >= deadline:
                        raise TimeoutError("proxy did not close the refused connection")
        if not receipts_done.wait(timeout=2):
            raise TimeoutError("did not observe all 128 receipt callbacks")
        print(json.dumps({
            "check": "queue with callback", "received_callbacks": dict(counts),
            "queued_events": egress._events.qsize(), "queue_limit": egress._events.maxsize,
        }))


def admin_probe(config, binary):
    requests = []

    class FakeProxy(BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.path)
            body = b'{"status":"forged-by-proxy","policy_generation":999}'
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    with HTTPServer(("127.0.0.1", 0), FakeProxy) as server:
        server.timeout = 3
        thread = threading.Thread(target=server.handle_request, daemon=True)
        thread.start()
        try:
            with patch.dict(os.environ, {
                "http_proxy": f"http://127.0.0.1:{server.server_port}",
                "no_proxy": "audit.invalid",
            }, clear=True), patch.object(urllib.request, "_opener", None):
                # Isolate _admin_get: no admin daemon is started for this case.
                egress = EgressProcess(config, binary=binary)
                egress.admin_port = 9
                try:
                    result = egress.health()
                except Exception as error:
                    result = {"error_type": type(error).__name__}
                print(json.dumps({
                    "check": "admin proxy interception", "result": result,
                    "proxy_received": requests,
                }))
        finally:
            thread.join(timeout=4)
            if thread.is_alive():
                raise TimeoutError("local proxy thread did not stop")


if __name__ == "__main__":
    binary = os.environ["MAELYS_EGRESS_BINARY"]
    if not Path(binary).is_absolute():
        raise ValueError("MAELYS_EGRESS_BINARY must be an absolute path")
    config = EgressConfig([Destination("127.0.0.1", 9, allow_private=True)])
    queue_probe(config, binary)
    admin_probe(config, binary)
