import io
import json
import os
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import queue
import stat
import threading
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).parents[1] / "src"))

import maelys_egress
from maelys_egress import (
    Destination, EgressConfig, EgressProcess, binary_trust_refusal,
)


class ProcessSdkTest(unittest.TestCase):
    def test_ipv6_urls_are_bracketed(self) -> None:
        process = EgressProcess(
            EgressConfig(
                [Destination("127.0.0.1", 9, allow_private=True)],
                listen_host="::1",
                admin_host="::1",
            )
        )
        process.secret = "secret"
        process.proxy_port = 8080
        self.assertEqual(process.proxy_url, "http://maelys:secret@[::1]:8080")

    def test_lifecycle_health_metrics_and_reload(self) -> None:
        binary = os.environ["MAELYS_EGRESS_BINARY"]
        config = EgressConfig([Destination("127.0.0.1", 9, allow_private=True)])
        with EgressProcess(config, binary=binary, stderr=None) as egress:
            ready = egress.next_event(timeout=1.0)
            self.assertEqual(ready["contract"], "maelys-egress-lifecycle/1")
            self.assertEqual(ready["event"], "ready")
            self.assertEqual(stat.S_IMODE(egress.directory.stat().st_mode), 0o700)
            self.assertEqual(
                stat.S_IMODE((egress.directory / "token").stat().st_mode), 0o600
            )
            self.assertEqual(
                stat.S_IMODE((egress.directory / "egress.conf").stat().st_mode), 0o600
            )
            self.assertNotIn(egress.secret, " ".join(egress.process.args))
            self.assertEqual(egress.health()["status"], "ok")
            self.assertIn("maelys:", egress.proxy_url)
            self.assertEqual(
                int(egress.metrics()["maelys_egress_policy_generation"]), 1
            )
            generation = egress.replace_destinations(
                [Destination("127.0.0.1", 8, allow_private=True)]
            )
            self.assertEqual(generation, 2)
            reloaded = egress.next_event(timeout=1.0)
            self.assertEqual(reloaded["event"], "policy-reloaded")

    def test_invalid_configuration_and_failed_start_cleanup(self) -> None:
        with self.assertRaises(ValueError):
            EgressConfig([])
        with self.assertRaises(ValueError):
            Destination(
                "127.0.0.1", 443, allow_private=True, require_tls_sni=True
            ).directive()
        with self.assertRaisesRegex(ValueError, "absolute path"):
            EgressProcess(
                EgressConfig([Destination("127.0.0.1", 9, allow_private=True)]),
                binary="maelys-egress",
            )
        process = EgressProcess(
            EgressConfig([Destination("127.0.0.1", 9, allow_private=True)]),
            binary="/definitely/missing/maelys-egress",
        )
        with self.assertRaises(FileNotFoundError):
            process.start()
        self.assertIsNone(process.directory)
        process.close()

    def test_discovery_refuses_a_replaceable_binary(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bin_dir = Path(directory) / "bin"
            bin_dir.mkdir(mode=0o755)
            binary = bin_dir / "maelys-egress"
            binary.write_text("#!/bin/sh\nexit 0\n")
            binary.chmod(0o755)
            with mock.patch.object(
                maelys_egress, "_discovery_candidates", lambda: (binary,)
            ):
                # Owned by the caller, writable by nobody else: trusted.
                self.assertIsNone(binary_trust_refusal(binary))
                self.assertEqual(
                    maelys_egress._resolve_binary(None), str(binary.resolve())
                )
                # The owner's group is the owner's decision.
                binary.chmod(0o775)
                self.assertIsNone(binary_trust_refusal(binary))
                # Anyone may replace the file: refused, and the refusal is named.
                binary.chmod(0o777)
                self.assertIn("writable by everyone", binary_trust_refusal(binary))
                with self.assertRaisesRegex(FileNotFoundError, "writable by everyone"):
                    maelys_egress._resolve_binary(None)
                # Anyone may replace the directory entry: refused as well.
                binary.chmod(0o755)
                bin_dir.chmod(0o777)
                self.assertIn(str(bin_dir), binary_trust_refusal(binary))
                with self.assertRaises(FileNotFoundError):
                    maelys_egress._resolve_binary(None)
                # A sticky world-writable directory only lets owners replace
                # their own entries: trusted.
                bin_dir.chmod(0o1777)
                self.assertIsNone(binary_trust_refusal(binary))
                # A sticky file gets no exemption.
                binary.chmod(0o1777)
                self.assertIn("writable by everyone", binary_trust_refusal(binary))
                binary.chmod(0o755)
                bin_dir.chmod(0o755)
    def test_administration_ignores_the_proxy_environment(self) -> None:
        class Handler(BaseHTTPRequestHandler):
            body = b""
            status = 200

            def do_GET(self) -> None:  # noqa: N802 - http.server contract
                self.send_response(self.status)
                self.send_header("Content-Type", "application/json")
                if self.status != 200:
                    self.send_header("Location", "http://127.0.0.1:9/healthz")
                self.end_headers()
                self.wfile.write(self.body)

            def log_message(self, *args: object) -> None:
                del args

        class Proxy(Handler):
            body = json.dumps({"status": "forged-by-proxy"}).encode()

        class Admin(Handler):
            body = json.dumps({"status": "ok", "policy_generation": 1}).encode()

        servers = [HTTPServer(("127.0.0.1", 0), Proxy), HTTPServer(("127.0.0.1", 0), Admin)]
        threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in servers]
        for thread in threads:
            thread.start()
        proxy_url = f"http://127.0.0.1:{servers[0].server_port}"
        environment = {
            "HTTP_PROXY": proxy_url, "http_proxy": proxy_url,
            "ALL_PROXY": proxy_url, "all_proxy": proxy_url,
        }
        try:
            process = EgressProcess(
                EgressConfig([Destination("127.0.0.1", 9, allow_private=True)])
            )
            process.admin_port = servers[1].server_port
            with mock.patch.dict(os.environ, environment):
                for name in ("NO_PROXY", "no_proxy"):
                    os.environ.pop(name, None)
                self.assertEqual(process.health()["status"], "ok")
                # A redirect is an error, never followed.
                Admin.status = 302
                with self.assertRaisesRegex(RuntimeError, "replied 302"):
                    process.health()
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()
    @staticmethod
    def _feed(process: EgressProcess, count: int) -> None:
        lines = b"".join(
            json.dumps({
                "schemaVersion": 1, "contract": "maelys-egress-lifecycle/1",
                "event": "receipt", "sequence": index,
            }).encode() + b"\n"
            for index in range(count)
        )
        process.process = types.SimpleNamespace(stdout=io.BytesIO(lines))  # type: ignore[assignment]
        process._consume_lifecycle()
        process.process = None

    def test_event_retention_is_bounded(self) -> None:
        config = EgressConfig([Destination("127.0.0.1", 9, allow_private=True)])
        with self.assertRaises(ValueError):
            EgressProcess(config, max_pending_events=0)

        # No consumer: the oldest events are dropped, the newest kept, counted.
        process = EgressProcess(config, max_pending_events=4)
        self._feed(process, 10)
        self.assertEqual(process.pending_events, 4)
        self.assertEqual(process.dropped_events, 6)
        self.assertEqual(
            [process.next_event(timeout=0.0)["sequence"] for _ in range(4)], [6, 7, 8, 9]
        )
        with self.assertRaises(queue.Empty):
            process.next_event(timeout=0.01)

        # Slow consumer: a partial read keeps the bound and the order.
        self._feed(process, 3)
        self.assertEqual(process.next_event(timeout=0.0)["sequence"], 0)
        self._feed(process, 4)
        self.assertEqual(process.pending_events, 4)
        self.assertEqual(process.dropped_events, 6 + 2)
        self.assertEqual(
            [process.next_event(timeout=0.0)["sequence"] for _ in range(4)], [0, 1, 2, 3]
        )

        # Callback only: the callback is the consumer, nothing is retained.
        seen = []
        process = EgressProcess(config, on_event=seen.append, max_pending_events=4)
        self._feed(process, 10)
        self.assertEqual(len(seen), 10)
        self.assertEqual(process.pending_events, 0)
        self.assertEqual(process.dropped_events, 0)
        with self.assertRaisesRegex(RuntimeError, "on_event consumes"):
            process.next_event(timeout=0.0)


if __name__ == "__main__":
    unittest.main()
