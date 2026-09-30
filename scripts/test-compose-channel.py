#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Build this checkout, run the native Compose gate, remove only its own resources."""
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_schemas import load, validate

SCHEMA = load(ROOT / "protocol/egress-lifecycle-v1.schema.json")


def verify_containers(app, broker, upstream):
    """Inspect actual Docker state, not merely the intended YAML."""
    settings = app["HostConfig"]
    assert settings["NetworkMode"] == "none", "application acquired ambient networking"
    assert settings["ReadonlyRootfs"] and not settings["Privileged"]
    assert settings["CapDrop"] == ["ALL"] and not settings["CapAdd"]
    assert "no-new-privileges:true" in settings["SecurityOpt"]
    assert app["Config"]["User"] == "10002:10002"
    assert settings["GroupAdd"] == ["20000"]
    assert broker["Config"]["User"] == "10001:20000", "broker/app identities must differ"
    for network in app["NetworkSettings"]["Networks"].values():
        assert not network["IPAddress"] and not network["GlobalIPv6Address"]
    assert len(app["Mounts"]) == 1, "application has a mount other than its capability"
    capability = app["Mounts"][0]
    assert capability["Type"] == "volume" and capability["Destination"] == "/run/maelys-egress"
    assert capability["RW"] is False
    shared = next(m for m in broker["Mounts"] if m["Destination"] == "/run/maelys-egress")
    assert shared["RW"] and shared["Name"] == capability["Name"]
    for entry in app["Config"]["Env"]:
        key, _, value = entry.partition("=")
        assert not (key.lower().endswith("_proxy") and value), "proxy configuration leaked into app"
        assert not any(word in key.lower() for word in ("token", "secret", "password", "credential"))
    assert app["Config"]["Entrypoint"] == ["/usr/local/bin/native-app"]
    addresses = {network["IPAddress"] for network in upstream["NetworkSettings"]["Networks"].values()}
    assert addresses and "" not in addresses
    assert not settings["PortBindings"]
    return addresses


def verify_outputs(app_text, broker_text, addresses):
    reports = [json.loads(line) for line in app_text.splitlines()]
    assert [r["check"] for r in reports] == ["deployment", "native-allowed", "direct-denied",
        "policy-denied", "lease-independent-stream", "complete"]
    assert all(r["ok"] is True for r in reports)
    assert reports[2]["address"] in addresses and reports[2]["port"] == 8080
    events = [json.loads(line) for line in broker_text.splitlines()]
    for event in events:
        assert not validate(event, SCHEMA), event
    names = [event["event"] for event in events]
    assert names[0] == "ready" and names.count("ready") == 1 and names[-1] == "stopped", names
    assert "fatal" not in names and names.count("stopping") == 1, names
    assert "proxy" not in events[0]
    assert events[0]["channel"] == {"transport": "unix", "path": "/run/maelys-egress/channel.sock",
                                      "protocol": "maelys-egress-channel-bootstrap/1"}
    receipts = [event["receipt"] for event in events if event["event"] == "receipt"]
    assert len(receipts) == 2 and len({r["id"] for r in receipts}) == 2, receipts
    assert {(r["port"], r["result"]) for r in receipts} == {(8080, "ok"), (8081, "denied")}, receipts
    for receipt in receipts:
        assert receipt["principal"] == "compose-agent" and receipt["invocationId"] == "native-compose"
        assert receipt["host"] == "upstream"
    allowed = next(r for r in receipts if r["port"] == 8080)
    assert allowed["policyGeneration"] == events[0]["policy"]["generation"] == 1
    assert allowed["policyDigest"] == events[0]["policy"]["digest"]
    assert allowed["bytesFromClient"] == 2 * len(b"native-compose\n")
    assert allowed["bytesToClient"] == 2 * len(f"native-compose {reports[2]['address']}\n".encode("ascii"))
    denied = next(r for r in receipts if r["port"] == 8081)
    assert denied["bytesFromClient"] == denied["bytesToClient"] == 0


def main():
    if not __debug__:
        raise SystemExit("run the integration gate without Python optimization")
    dependencies = Path(os.environ["MAELYS_DEPENDENCIES_DIR"]).resolve()
    project = "egress-channel-test-" + uuid.uuid4().hex[:12]
    images = [project + ":build", project + ":egress", project + ":app"]
    env = dict(os.environ, MAELYS_EGRESS_IMAGE=images[1],
               MAELYS_EGRESS_APP_IMAGE=images[2])
    compose_command = ["docker", "compose", "--project-name", project,
                       "--file", str(ROOT / "examples/compose-channel/compose.yaml")]

    def run(command, *, capture=False, timeout=120, check=True):
        return subprocess.run(command, env=env, text=True, capture_output=capture,
                              timeout=timeout, check=check)

    def compose(*args, **options):
        return run([*compose_command, *args], **options)

    def inspect(identifier):
        return json.loads(run(["docker", "inspect", identifier], capture=True).stdout)[0]

    contexts = []
    for name in ("maelys-system", "maelys-cli", "agent-cli-spec"):
        directory = dependencies / name
        assert directory.is_dir(), f"missing pinned checkout: {name}"
        contexts.extend(["--build-context", f"{name}={directory}"])
    build = ["docker", "build", *contexts, "--file", str(ROOT / "docker/Dockerfile.sidecar")]
    try:
        # Always build this checkout: a same-named image from yesterday must
        # never stand in for the code whose gate is being evaluated.
        run([*build, "--target", "build", "--tag", images[0], str(ROOT)], timeout=900)
        run([*build, "--tag", images[1], str(ROOT)], timeout=900)
        compose("config", "--quiet")
        # Use the same docker build driver for the local SDK and its consumer.
        # Compose may select a separate buildx builder without that local image.
        run(["docker", "build", "--build-arg", f"MAELYS_EGRESS_BUILD_IMAGE={images[0]}",
             "--file", str(ROOT / "examples/compose-channel/Dockerfile.app"), "--tag", images[2],
             str(ROOT / "examples/compose-channel")], timeout=300)
        compose("up", "--detach", "--no-build", timeout=180)
        identifiers = {service: compose("ps", "--all", "--quiet", service, capture=True).stdout.strip()
                       for service in ("app", "egress", "upstream")}
        assert all(identifiers.values()), "Compose did not create every service"
        status = run(["docker", "wait", identifiers["app"]], capture=True, timeout=45).stdout.strip()
        assert status == "0", f"native application exited {status}"
        state = {service: inspect(identifier) for service, identifier in identifiers.items()}
        addresses = verify_containers(state["app"], state["egress"], state["upstream"])
        assert state["egress"]["State"]["Running"] and state["upstream"]["State"]["Running"]
        modes = compose("exec", "-T", "egress", "stat", "-c", "%u:%g:%a",
            "/run/maelys-egress", "/run/maelys-egress/channel.sock", capture=True).stdout.splitlines()
        assert modes == ["10001:20000:2750", "10001:20000:660"], modes
        # Drain the server cleanly so every receipt and final lifecycle event
        # must already exist; no sleep-based guess about log delivery.
        compose("stop", "--timeout", "10", "egress", timeout=20)
        assert inspect(identifiers["egress"])["State"]["ExitCode"] == 0
        app_logs = run(["docker", "logs", identifiers["app"]], capture=True)
        broker_logs = run(["docker", "logs", identifiers["egress"]], capture=True)
        assert not app_logs.stderr and not broker_logs.stderr
        verify_outputs(app_logs.stdout, broker_logs.stdout, addresses)
        print(app_logs.stdout, end="")
        print("compose-channel-check: native allowed, direct unreachable, policy denied; identity, isolation and receipts verified")
    except BaseException:
        compose("logs", "--no-color", check=False, timeout=20)
        raise
    finally:
        # These names contain a fresh random project ID; never remove the
        # user's manual example project, volumes or pre-existing image tags.
        compose("down", "--volumes", "--remove-orphans", check=False, timeout=30)
        run(["docker", "image", "rm", *images], capture=True, check=False, timeout=30)


if __name__ == "__main__":
    main()
