#!/usr/bin/env python3
"""Validate what the binary actually emits against the committed schemas.

Finite envelopes are checked against protocol/agent-cli-v2.schema.json and
their `data` against cli/schemas/*.json; every line of a real `serve` run
(ready, receipt, reload, rejected reload, stopping, stopped) is checked
against protocol/egress-lifecycle-v1.schema.json. No third-party package:
the validator below covers the JSON Schema subset these files use.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def type_matches(value: object, expected: str) -> bool:
    return {
        "object": lambda v: isinstance(v, dict),
        "array": lambda v: isinstance(v, list),
        "string": lambda v: isinstance(v, str),
        "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
        "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
        "boolean": lambda v: isinstance(v, bool),
        "null": lambda v: v is None,
    }[expected](value)


def validate(value: object, schema: dict, path: str = "$") -> list[str]:
    errors: list[str] = []
    if "type" in schema:
        expected = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(type_matches(value, t) for t in expected):
            return [f"{path}: expected type {expected}"]
    if "const" in schema and value != schema["const"]:
        errors.append(f"{path}: expected const {schema['const']!r}, got {value!r}")
    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{path}: {value!r} not in enum")
    if isinstance(value, str):
        if "minLength" in schema and len(value) < schema["minLength"]:
            errors.append(f"{path}: shorter than {schema['minLength']}")
        if "pattern" in schema and not re.search(schema["pattern"], value):
            errors.append(f"{path}: does not match {schema['pattern']}")
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append(f"{path}: below minimum {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]:
            errors.append(f"{path}: above maximum {schema['maximum']}")
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                errors.append(f"{path}: missing required {key}")
        properties = schema.get("properties", {})
        for key, item in value.items():
            if key in properties:
                errors.extend(validate(item, properties[key], f"{path}.{key}"))
            elif schema.get("additionalProperties") is False:
                errors.append(f"{path}: unexpected property {key}")
    if isinstance(value, list) and "items" in schema:
        for index, item in enumerate(value):
            errors.extend(validate(item, schema["items"], f"{path}[{index}]"))
    for sub in schema.get("allOf", []):
        errors.extend(validate(value, sub, path))
    if "oneOf" in schema:
        matches = [sub for sub in schema["oneOf"] if not validate(value, sub, path)]
        if len(matches) != 1:
            errors.append(f"{path}: {len(matches)} oneOf branches matched")
    if "not" in schema and not validate(value, schema["not"], path):
        errors.append(f"{path}: matches forbidden schema")
    if "if" in schema:
        branch = "then" if not validate(value, schema["if"], path) else "else"
        if branch in schema:
            errors.extend(validate(value, schema[branch], path))
    return errors


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def fail(message: str) -> None:
    raise SystemExit(f"schema-check: {message}")


def check(label: str, value: object, schema: dict) -> None:
    errors = validate(value, schema)
    if errors:
        fail(f"{label}: " + "; ".join(errors))


def run(binary: Path, *arguments: str) -> tuple[int, str, str]:
    completed = subprocess.run(
        [str(binary), *arguments, "--format", "json", "--compact", "--non-interactive"],
        text=True, capture_output=True,
    )
    return completed.returncode, completed.stdout, completed.stderr


def check_finite(binary: Path, envelope_schema: dict, data_schemas: dict, config: Path,
                 broken: Path, missing: Path) -> None:
    cases = [
        ("describe --summary", ("describe", "--summary"), 0, None),
        ("config describe", ("config", "describe"), 0, "config-describe"),
        ("config validate (valid)", ("config", "validate", "--config", str(config)), 0,
         "config-validate"),
        ("config validate (violations)", ("config", "validate", "--config", str(broken)), 2,
         "config-validate"),
        ("config validate (missing file)", ("config", "validate", "--config", str(missing)),
         1, None),
        ("completion zsh", ("completion", "zsh"), 0, "completion"),
    ]
    for label, arguments, expected_status, data_schema in cases:
        status, out, err = run(binary, *arguments)
        if status != expected_status:
            fail(f"{label}: exit {status}, expected {expected_status}")
        if status == 1:
            if out:
                fail(f"{label}: stdout must be empty on failure")
            envelope = json.loads(err)
        else:
            if err:
                fail(f"{label}: stderr must be empty on success")
            envelope = json.loads(out)
        check(f"{label} envelope", envelope, envelope_schema)
        if envelope.get("exitCode") != status:
            fail(f"{label}: exitCode {envelope.get('exitCode')} differs from {status}")
        if data_schema and status != 1:
            check(f"{label} data", envelope["data"], data_schemas[data_schema])
    print("schema-check: finite envelopes and data conform")


def check_lifecycle(binary: Path, schema: dict, config: Path) -> None:
    process = subprocess.Popen(
        [str(binary), "serve", "--config", str(config), "--non-interactive"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    events: list[dict] = []
    ready = threading.Event()
    seen = threading.Condition()

    def drain() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            event = json.loads(line)
            with seen:
                events.append(event)
                seen.notify_all()
            if event.get("event") == "ready":
                ready.set()

    thread = threading.Thread(target=drain, daemon=True)
    thread.start()

    def wait_for(name: str, timeout: float = 5.0) -> None:
        deadline = time.monotonic() + timeout
        with seen:
            while not any(e.get("event") == name for e in events):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    fail(f"lifecycle: no {name} event within {timeout}s")
                seen.wait(remaining)

    try:
        if not ready.wait(5.0):
            fail("lifecycle: daemon did not become ready")
        port = next(e for e in events if e["event"] == "ready")["proxy"]["port"]
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as client:
            client.sendall(b"CONNECT 127.0.0.1:9 HTTP/1.1\r\nHost: 127.0.0.1:9\r\n\r\n")
            try:
                client.recv(256)
            except OSError:
                pass
        wait_for("receipt")
        text = config.read_text(encoding="utf-8").replace("127.0.0.1:9", "127.0.0.1:8")
        config.write_text(text, encoding="utf-8")
        process.send_signal(signal.SIGHUP)
        wait_for("policy-reloaded")
        config.write_text(text + "listen = 127.0.0.1:1\n", encoding="utf-8")
        process.send_signal(signal.SIGHUP)
        wait_for("policy-reload-rejected")
    finally:
        process.send_signal(signal.SIGTERM)
        status = process.wait(timeout=5.0)
        thread.join(timeout=2.0)
    if status != 0:
        fail(f"lifecycle: serve exited with {status}")
    if process.stderr is not None and process.stderr.read():
        fail("lifecycle: serve wrote to stderr")
    for index, event in enumerate(events):
        check(f"lifecycle line {index + 1} ({event.get('event')})", event, schema)
    names = [e["event"] for e in events]
    expected = ["ready", "receipt", "policy-reloaded", "policy-reload-rejected",
                "stopping", "stopped"]
    for name in expected:
        if name not in names:
            fail(f"lifecycle: event {name} was never emitted")
    if names[0] != "ready" or names[-2:] != ["stopping", "stopped"]:
        fail(f"lifecycle: unexpected order {names}")
    print(f"schema-check: {len(events)} lifecycle events conform")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    arguments = parser.parse_args()
    envelope_schema = load(ROOT / "protocol" / "agent-cli-v2.schema.json")
    lifecycle_schema = load(ROOT / "protocol" / "egress-lifecycle-v1.schema.json")
    data_schemas = {path.stem: load(path) for path in (ROOT / "cli" / "schemas").glob("*.json")}
    with tempfile.TemporaryDirectory(prefix="maelys-egress-schema-") as directory:
        root = Path(directory)
        os.chmod(root, 0o700)
        config = root / "egress.conf"
        config.write_text(
            "schema_version = 1\nlisten = 127.0.0.1:0\nadmin_listen = 127.0.0.1:0\n"
            "unauthenticated_loopback = true\nallow_private = 127.0.0.1:9\n",
            encoding="utf-8",
        )
        os.chmod(config, 0o600)
        broken = root / "broken.conf"
        broken.write_text(config.read_text(encoding="utf-8") + "max_connections = 0\n",
                          encoding="utf-8")
        os.chmod(broken, 0o600)
        check_finite(arguments.binary, envelope_schema, data_schemas, config, broken,
                     root / "missing.conf")
        check_lifecycle(arguments.binary, lifecycle_schema, config)
    return 0


if __name__ == "__main__":
    sys.exit(main())
