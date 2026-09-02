import os
from pathlib import Path
import stat
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "src"))

from maelys_egress import Destination, EgressConfig, EgressProcess


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
        process = EgressProcess(
            EgressConfig([Destination("127.0.0.1", 9, allow_private=True)]),
            binary="/definitely/missing/maelys-egress",
        )
        with self.assertRaises(FileNotFoundError):
            process.start()
        self.assertIsNone(process.directory)
        process.close()


if __name__ == "__main__":
    unittest.main()
