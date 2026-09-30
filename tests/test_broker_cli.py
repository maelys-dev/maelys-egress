#!/usr/bin/env python3
"""Exercise the declared broker command with the standalone C client."""
import http.client
import json
import os
from pathlib import Path
import queue
import signal
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_schemas import load, validate

BINARY = Path(sys.argv[1]).resolve()
CLIENT = BINARY.parent / "bootstrap-cli-client"
SCHEMA = load(ROOT / "protocol/egress-lifecycle-v1.schema.json")
ENV = dict(os.environ, MAELYS_CLI_FORMAT="json")


def invoke(*args, status=0):
    run = subprocess.run([str(BINARY), *args, "--non-interactive"], env=ENV,
                         capture_output=True, text=True, timeout=10)
    assert run.returncode == status, (args, run.returncode, run.stdout, run.stderr)
    if status == 1:
        assert not run.stdout, run.stdout
        value = json.loads(run.stderr)
        assert value["ok"] is False and value["error"]["code"]
    else:
        assert not run.stderr, run.stderr
        value = json.loads(run.stdout)
        assert value["ok"] is True
    return value


class Daemon:
    def __init__(self, config):
        self.events = []
        self.lines = queue.Queue()
        self.process = subprocess.Popen(
            [str(BINARY), "channel", "broker", "--config", str(config), "--non-interactive"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=ENV)
        def drain():
            for line in self.process.stdout:
                self.lines.put(line)
            self.lines.put(None)
        self.reader = threading.Thread(target=drain, daemon=True)
        self.reader.start()

    def event(self, name):
        for _ in range(100):
            line = self.lines.get(timeout=8)
            assert line is not None, (self.process.poll(), self.events)
            event = json.loads(line)
            assert not validate(event, SCHEMA), (event, validate(event, SCHEMA))
            self.events.append(event)
            if event["event"] == name:
                return event
        raise AssertionError(f"no {name} event")

    def finish(self, status=0):
        assert self.process.wait(timeout=8) == status
        self.reader.join(timeout=2)
        assert not self.process.stderr.read()

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        self.reader.join(timeout=2)
        self.process.stdout.close()
        self.process.stderr.close()


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            block = self.request.recv(4096)
            if not block:
                return
            self.request.sendall(block)


def run():
    catalog = invoke("describe", "--summary")["data"]["commands"]
    assert sum(c["id"] == "channel.broker" for c in catalog) == 1
    command = invoke("describe", "channel.broker")["data"]["commands"][0]
    assert command["purpose"] == (
        "Serve native channels through a private Unix socket until SIGINT or SIGTERM; "
        "stdout carries the lifecycle JSON Lines stream."), command
    assert next(c for c in catalog if c["id"] == "channel.broker")["purpose"] == command["purpose"]
    assert command["effect"] == "stream" and command["outputMode"] == "protocol-stream"
    assert command["protocol"] == "maelys-egress-lifecycle/1"
    assert [o["long"] for o in command["input"]["options"]] == ["--config"]
    keys = {k["name"]: k for k in invoke("config", "describe")["data"]["keys"]}
    assert keys["channel_max_clients"]["default"] == "128"
    assert keys["channel_connect_timeout_ms"]["default"] == "5000"
    assert keys["channel_handshake_timeout_ms"]["default"] == "5000"
    assert "tls_cert" not in keys or keys["tls_cert"]["type"] == "path"
    for args in [("channel", "broker"), ("channel", "exec"),
                 ("channel", "broker", "--config", "absent", "--listen", "127.0.0.1:0"),
                 ("channel", "broker", "--config", "absent", "--config", "absent"),
                 ("channel", "broker", "--config", "absent", "--format", "json")]:
        invoke(*args, status=1)

    with tempfile.TemporaryDirectory(prefix="egress-broker-cli-", dir="/tmp") as temporary:
        directory = Path(temporary).resolve()
        os.chmod(directory, 0o700)
        config, path = directory / "egress.conf", directory / "channel.sock"
        base = (f"schema_version = 1\nchannel_listen_unix = {path}\n"
                "channel_principal = agent-01\nchannel_invocation_id = run-42\n"
                "allow_private = 127.0.0.1:9\n")
        config.write_text(base)
        assert invoke("config", "validate", "--config", str(config))["data"]["valid"]
        assert not path.exists()
        mismatch = invoke("serve", "--config", str(config), status=1)
        assert mismatch["error"]["code"] == "VALIDATION_FAILED" and not path.exists()
        for line in ["listen = 127.0.0.1:0", "listen_unix = /tmp/proxy.sock",
                     "unix_peer = authenticated", "token_file = /absent",
                     "unauthenticated_loopback = false", "tls_cert = /absent",
                     "require_client_cert = false", "channel_max_clients = 0",
                     "channel_max_clients = 4097", "channel_connect_timeout_ms = 600001",
                     "channel_handshake_timeout_ms = 0", "channel_handshake_timeout_ms = 60001",
                     "channel_max_clients = 1\nchannel_max_clients = 2"]:
            config.write_text(base + line + "\n")
            assert not invoke("config", "validate", "--config", str(config), status=2)["data"]["valid"]
        for invalid in [base.replace("channel_principal = agent-01\n", ""),
                        "schema_version = 1\nallow = example.invalid:443\nchannel_max_clients = 128\n",
                        base.replace("agent-01", "invalid identity"),
                        base.replace("agent-01", "x" * 64),
                        base.replace(str(path), str(directory / ".." / "other.sock"))]:
            config.write_text(invalid)
            assert not invoke("config", "validate", "--config", str(config), status=2)["data"]["valid"]
        config.write_text("schema_version = 1\nunauthenticated_loopback = true\nallow_private = 127.0.0.1:9\n")
        assert invoke("channel", "broker", "--config", str(config), status=1)["error"]["code"] == "VALIDATION_FAILED"
        config.write_text(base)
        path.write_text("do not replace")
        invoke("channel", "broker", "--config", str(config), status=1)
        assert path.read_text() == "do not replace"
        path.unlink()

        servers = [socketserver.ThreadingTCPServer(("127.0.0.1", 0), Echo) for _ in range(2)]
        for server in servers:
            server.daemon_threads = True
            threading.Thread(target=server.serve_forever, daemon=True).start()
        daemon, client = None, None
        try:
            ports = [s.server_address[1] for s in servers]
            audit_key, audit_log = directory / "audit.key", directory / "audit.jsonl"
            audit_key.write_text("0123456789abcdef0123456789abcdef")
            os.chmod(audit_key, 0o600)
            config_text = base.replace("127.0.0.1:9", f"127.0.0.1:{ports[0]}") + (
                "admin_listen = 127.0.0.1:0\nquota_connections = 1\nquota_bytes = 4096\n"
                "quota_total_bytes = 16\nchannel_connect_timeout_ms = 777\nchannel_max_clients = 2\n"
                f"audit_log = {audit_log}\naudit_key_file = {audit_key}\naudit_key_id = test-key\n")
            config.write_text(config_text)
            assert invoke("config", "validate", "--config", str(config))["data"]["valid"]
            assert not audit_log.exists() and not path.exists()
            daemon = Daemon(config)
            ready = daemon.event("ready")
            assert len(daemon.events) == 1 and "proxy" not in ready
            assert ready["channel"] == {"transport": "unix", "path": str(path),
                                        "protocol": "maelys-egress-channel-bootstrap/1"}
            assert (path.stat().st_mode & 0o777) == 0o600
            connection = http.client.HTTPConnection("127.0.0.1", ready["admin"]["port"], timeout=2)
            connection.request("GET", "/healthz")
            assert connection.getresponse().status == 200
            connection.close()
            client = subprocess.Popen([str(CLIENT), str(path)], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            answers = queue.Queue()
            def read_client():
                for line in client.stdout:
                    answers.put(line.strip())
                answers.put(None)
            reader = threading.Thread(target=read_client, daemon=True)
            reader.start()
            assert answers.get(timeout=5) == "ready 777"
            def request(port, expected):
                client.stdin.write(f"127.0.0.1 {port}\n")
                client.stdin.flush()
                assert answers.get(timeout=6) == expected
                receipt = daemon.event("receipt")["receipt"]
                assert receipt["principal"] == "agent-01" and receipt["invocationId"] == "run-42"
                return receipt
            receipt = request(ports[0], "ok")
            assert receipt["quota"]["executionMax"] == 16 and receipt["quota"]["connectionMax"] == 4096
            request(ports[1], "denied")
            # Identity and every broker control key are immutable on reload.
            for old, new in [("agent-01", "agent-02"), ("run-42", "run-43"),
                             ("= 777", "= 778"), ("channel_max_clients = 2", "channel_max_clients = 3"),
                             (str(path), str(directory / "other.sock"))]:
                config.write_text(config_text.replace(old, new))
                daemon.process.send_signal(signal.SIGHUP)
                daemon.event("policy-reload-rejected")
            config.write_text(config_text + "channel_handshake_timeout_ms = 1234\n")
            daemon.process.send_signal(signal.SIGHUP)
            daemon.event("policy-reload-rejected")
            config.write_text(config_text.replace(f"127.0.0.1:{ports[0]}", f"127.0.0.1:{ports[1]}"))
            daemon.process.send_signal(signal.SIGHUP)
            assert daemon.event("policy-reloaded")["policy"]["generation"] == 2
            request(ports[0], "denied")
            assert request(ports[1], "ok")["policyGeneration"] == 2
            request(ports[1], "denied")  # Cumulative principal budget exhausted.
            client.stdin.close()
            assert client.wait(timeout=5) == 0 and not client.stderr.read()
            daemon.process.send_signal(signal.SIGTERM)
            daemon.event("stopping")
            daemon.event("stopped")
            daemon.finish()
            assert not path.exists()
            records = [json.loads(line) for line in audit_log.read_text().splitlines()]
            assert records and all("mac" in record for record in records)
            assert "agent-01" in audit_log.read_text() and "generation=2" in audit_log.read_text()
            # Schema also rejects ambiguous readiness, not just valid examples.
            assert validate(dict(ready, proxy={"transport": "tcp", "host": "127.0.0.1", "port": 1}), SCHEMA)
        finally:
            if client:
                if client.poll() is None:
                    client.kill()
                    client.wait(timeout=2)
                for stream in (client.stdin, client.stdout, client.stderr):
                    stream.close()
            if daemon:
                daemon.close()
            for server in servers:
                server.shutdown()
                server.server_close()

        # Runtime loss of the broker is fatal, not a silently idle CLI.
        config.write_text(base)
        daemon = Daemon(config)
        try:
            daemon.event("ready")
            os.chmod(directory, 0o750)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
                peer.settimeout(2)
                peer.connect(str(path))
            daemon.event("fatal")
            daemon.finish(status=1)
        finally:
            daemon.close()
            os.chmod(directory, 0o700)
        # Exercise the shutdown/liveness snapshot race in both signal modes.
        path.unlink(missing_ok=True)  # The failed inode cleanup deliberately retained this socket.
        for signum in (signal.SIGINT, signal.SIGTERM):
            for _ in range(5):
                daemon = Daemon(config)
                try:
                    daemon.event("ready")
                    daemon.process.send_signal(signum)
                    daemon.event("stopping")
                    daemon.event("stopped")
                    daemon.finish()
                    assert not path.exists()
                finally:
                    daemon.close()
    print("broker CLI: contract, secret-free identity, quotas, reload, shutdown and fatal supervision passed")


if __name__ == "__main__":
    run()
