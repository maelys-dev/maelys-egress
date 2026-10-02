#!/usr/bin/env python3
"""Exercise `channel exec`: its contract, its refusals, the program's status,
descriptor, signals and environment, and the order of its shutdown."""
import json
import os
from pathlib import Path
import shutil
import signal
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

BINARY = Path(sys.argv[1]).resolve()
PROBE = BINARY.parent / "exec-probe"
EXAMPLE = BINARY.parent / "example-channel_client"
ENV = dict(os.environ, MAELYS_CLI_FORMAT="json")
PURPOSE = (
    "Start one program with a native channel on an inherited descriptor and exit "
    "with its status, 128 + signal if a signal ended it; stdout and stderr are the "
    "program's. Not a sandbox: the program keeps its own network access.")


def invoke(*args, status=0):
    run = subprocess.run([str(BINARY), *args, "--non-interactive"], env=ENV,
                         capture_output=True, text=True, timeout=60)
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


LIMIT = 60  # seconds for one launch; a sanitizer's runtime can take several to start
GROUPS = []


def launch(config, *program, env=ENV, **options):
    """Each launch has its own process group, so that whatever it leaves can be ended."""
    process = subprocess.Popen(
        [str(BINARY), "channel", "exec", "--config", str(config), "--non-interactive",
         "--", *map(str, program)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env,
        start_new_session=True, **options)
    GROUPS.append(process.pid)
    return process


def end_group(group):
    try:
        os.killpg(group, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def finish(process, stdin=None):
    """Nothing here waits without a bound: a launcher that does not return, or
    a process it left holding its output, is a failure of the test and never a
    hang of the gate."""
    try:
        return process.communicate(stdin, timeout=LIMIT)
    except subprocess.TimeoutExpired:
        returned = process.poll()
        end_group(process.pid)
        try:
            out, err = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            out, err = "", ""
        raise AssertionError((
            "the launcher did not return" if returned is None else
            f"the launcher exited {returned} but its output was still held open",
            process.args, out, err)) from None


def line(process, stream):
    """One line of a running launch, with the same bound."""
    box = []
    reader = threading.Thread(target=lambda: box.append(stream.readline()), daemon=True)
    reader.start()
    reader.join(LIMIT)
    if not box:
        end_group(process.pid)
        raise AssertionError(("no line from the launch", process.args))
    return box[0]


def run(config, *program, status=0, stdin=None, env=ENV):
    """A program that started: the status is its own, stderr stays empty."""
    process = launch(config, *program, env=env,
                     stdin=subprocess.PIPE if stdin is not None else None)
    out, err = finish(process, stdin)
    assert process.returncode == status, (program, process.returncode, out, err)
    assert not err, err
    return out


def refused(config, *program, code):
    """A program that never started: an envelope, exit 1, nothing on stdout."""
    process = launch(config, *program)
    out, err = finish(process)
    assert process.returncode == 1 and not out, (program, process.returncode, out, err)
    error = json.loads(err)
    assert error["ok"] is False and error["error"]["code"] == code, error
    return error["error"]


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            block = self.request.recv(4096)
            if not block:
                return
            self.request.sendall(block)


def contract():
    catalog = invoke("describe", "--summary")["data"]["commands"]
    assert sum(c["id"] == "channel.exec" for c in catalog) == 1
    command = invoke("describe", "channel.exec")["data"]["commands"][0]
    assert command["purpose"] == PURPOSE, command
    assert command["effect"] == "stream" and command["outputMode"] == "protocol-stream"
    # A command that only relays a program's stdio names no protocol.
    assert "protocol" not in command and command["external"] is False
    assert command["exitCodes"] == {"0": "command completed", "1": "execution failed",
                                    "2": "valid report with violations"}
    assert [o["long"] for o in command["input"]["options"]] == ["--config"]
    operands = command["input"]["operands"]
    assert [(o["name"], o["required"], o["variadic"]) for o in operands] == [
        ("program", True, False), ("argument", False, True)]
    assert operands[0]["type"] == "absolute-path" and "type" not in operands[1]
    keys = {k["name"]: k for k in invoke("config", "describe")["data"]["keys"]}
    assert keys["channel_fd"]["default"] == "4" and keys["channel_fd"]["range"] == "3..255"
    assert keys["channel_fd"]["requires"] == "channel_principal"
    assert keys["channel_fd"]["conflicts"] == "channel_listen_unix"
    by_path = keys["channel_exec_by_path"]
    assert (by_path["type"], by_path["default"], by_path["allowedValues"]) == (
        "boolean", "false", "true,false")
    assert by_path["requires"] == "channel_principal"
    assert by_path["conflicts"] == "channel_listen_unix"
    assert "requires" not in keys["channel_principal"]
    assert keys["channel_connect_timeout_ms"]["requires"] == "channel_principal"
    assert keys["channel_max_clients"]["requires"] == "channel_listen_unix,channel_principal"


def modes(directory, config):
    """Each key in each of the three modes, and each command against each mode."""
    base = "schema_version = 1\nallow_private = 127.0.0.1:9\n"
    exec_mode = base + "channel_principal = agent-01\n"
    broker = exec_mode + f"channel_listen_unix = {directory / 'channel.sock'}\n"
    proxy = base + "unauthenticated_loopback = true\n"

    def valid(text):
        config.write_text(text)
        return invoke("config", "validate", "--config", str(config))["data"]["valid"]

    def invalid(text):
        config.write_text(text)
        report = invoke("config", "validate", "--config", str(config), status=2)["data"]
        assert not report["valid"]
        return report["diagnostics"][0]["message"]

    for text in [exec_mode, broker, proxy,
                 exec_mode + "channel_fd = 3\n", exec_mode + "channel_fd = 255\n",
                 exec_mode + "channel_invocation_id = run-42\n",
                 exec_mode + "channel_connect_timeout_ms = 777\n",
                 exec_mode + "quota_connections = 1\nquota_total_bytes = 16\n",
                 exec_mode + "admin_listen = 127.0.0.1:0\n",
                 exec_mode + "channel_exec_by_path = true\n",
                 exec_mode + "channel_exec_by_path = false\n",
                 broker + "channel_max_clients = 2\nchannel_handshake_timeout_ms = 1000\n"]:
        assert valid(text), text
    for text, reason in [
            (exec_mode + "channel_fd = 2\n", "channel_fd must be in 3..255"),
            (exec_mode + "channel_fd = 256\n", "channel_fd must be in 3..255"),
            (exec_mode + "channel_fd = 4\nchannel_fd = 5\n", "duplicate key"),
            (exec_mode + "channel_max_clients = 2\n", "require channel_listen_unix"),
            (exec_mode + "channel_handshake_timeout_ms = 1000\n", "require channel_listen_unix"),
            (broker + "channel_fd = 4\n", "refuse channel_listen_unix"),
            (broker + "channel_exec_by_path = false\n", "refuse channel_listen_unix"),
            (exec_mode + "channel_exec_by_path = yes\n", "boolean must be true or false"),
            (proxy + "channel_fd = 4\n", "require channel_principal"),
            (proxy + "channel_exec_by_path = true\n", "require channel_principal"),
            (proxy + "channel_invocation_id = run-42\n", "require channel_principal"),
            (base + f"channel_listen_unix = {directory / 'channel.sock'}\n",
             "require channel_principal"),
            (exec_mode + "listen = 127.0.0.1:0\n", "refuse proxy listener"),
            (exec_mode + "token_file = /absent\n", "refuse proxy listener"),
            (exec_mode + "unauthenticated_loopback = false\n", "refuse proxy listener"),
            (exec_mode.replace("agent-01", "invalid identity"),
             "canonical native principal")]:
        assert reason in invalid(text), (text, reason)
    for text, own in [(exec_mode, "exec"), (broker, "broker"), (proxy, "serve")]:
        config.write_text(text)
        for command in ["exec", "broker", "serve"]:
            if command == own:
                continue
            args = (["channel", command] if command != "serve" else ["serve"]) + [
                "--config", str(config)]
            if command == "exec":
                args += ["--", str(PROBE), "exit", "0"]
            error = invoke(*args, status=1)["error"]
            assert error["code"] == "VALIDATION_FAILED", (own, command, error)
            assert error["message"] == "configuration mode does not match the command"
    return exec_mode


def test():
    contract()
    with tempfile.TemporaryDirectory(prefix="egress-exec-", dir="/tmp") as temporary:
        directory = Path(temporary).resolve()
        os.chmod(directory, 0o700)
        config = directory / "egress.conf"
        exec_mode = modes(directory, config)

        # Nothing started: an envelope, exit 1, and stdout untouched.
        config.write_text(exec_mode)
        refused(config, "exec-probe", "exit", "0", code="VALIDATION_FAILED")
        refused(config, directory / "absent", code="NOT_FOUND")
        writable = directory / "writable-probe"
        shutil.copy(PROBE, writable)
        os.chmod(writable, 0o777)
        refused(config, writable, "exit", "0", code="ACCESS_DENIED")
        plain = directory / "not-executable"
        plain.write_text("#!/bin/sh\n")
        os.chmod(plain, 0o600)
        refused(config, plain, code="ACCESS_DENIED")
        for args in [("channel", "exec"), ("channel", "exec", "--config", str(config)),
                     ("channel", "exec", "--", str(PROBE)),
                     ("channel", "exec", "--config", str(config), "--format", "json",
                      "--", str(PROBE), "exit", "0")]:
            invoke(*args, status=1)

        # Started: the status is the program's, 0 and 2 included, and Egress
        # adds nothing to either stream.
        for code in (0, 1, 2, 42, 255):
            assert run(config, PROBE, "exit", code, status=code) == ""
        assert run(config, PROBE, "cat", stdin="through\nthe launcher\n") == "through\nthe launcher\n"

        # The descriptor, the two variables and the arguments. Every number
        # from 3 to 12 is asked for in turn: one of them is the number the
        # kernel gave the launcher's own end, the case where the source is
        # its own target and close-on-exec must be cleared explicitly.
        for number in [None, *range(3, 13)]:
            config.write_text(exec_mode + "channel_connect_timeout_ms = 777\n" +
                              (f"channel_fd = {number}\n" if number else ""))
            lines = run(config, PROBE, "report", "--flag", "", "two words", "--").splitlines()
            expected = number or 4
            assert lines[:5] == [f"fd {expected}", "timeout 777", "type datagram", "cloexec 0",
                                 f"open {expected}"], (number, lines)
            assert lines[5:] == ["argument [--flag]", "argument []", "argument [two words]",
                                 "argument [--]"], lines
        # The checked program is executed through the descriptor held across
        # the check, and through its path only when the configuration asks:
        # the name the kernel gives the program says which, where the platform
        # executes through a descriptor at all. Either way the program runs
        # with its channel.
        for by_path in (False, True):
            config.write_text(exec_mode + f"channel_exec_by_path = {str(by_path).lower()}\n")
            name = run(config, PROBE, "execfn").strip()
            if sys.platform.startswith("linux") and by_path:
                assert name == f"execfn {PROBE}", name
            elif sys.platform.startswith("linux"):
                assert name.startswith("execfn /dev/fd/"), name
            else:
                assert name == "execfn unavailable", name
            assert run(config, PROBE, "report").splitlines()[0] == "fd 4"
        # The program the key exists for, where the system has one: a
        # multi-call binary that reads its applet from the name it was
        # executed under. Recent Ubuntu ships uutils as its coreutils, so
        # /bin/sleep is one there. Under the default it refuses and exits
        # non-zero, which is its status and not this command's; with the key
        # it runs. On a system without such a binary this proves nothing and
        # says so.
        sleeper = Path(os.path.realpath("/bin/sleep"))
        if sleeper.parent.name == "coreutils" and sleeper.name == "sleep":
            config.write_text(exec_mode)
            process = launch(config, "/bin/sleep", "0.1")
            out, err = finish(process)
            assert process.returncode != 0 and not out and err and not err.startswith("{"), (
                process.returncode, out, err)
            config.write_text(exec_mode + "channel_exec_by_path = true\n")
            assert run(config, "/bin/sleep", "0.1") == ""
            print("channel exec: a multi-call binary refuses the descriptor and runs by path")
        else:
            print("channel exec: no multi-call /bin/sleep here, channel_exec_by_path not exercised on one")
        config.write_text(exec_mode + "channel_connect_timeout_ms = 777\nchannel_fd = 12\n")
        environment = dict(ENV, MAELYS_EGRESS_CHANNEL_FD="99", KEPT="kept")
        assert run(config, "/bin/sh", "-c", 'echo "$MAELYS_EGRESS_CHANNEL_FD $KEPT"',
                   env=environment) == "12 kept\n"

        servers = [socketserver.ThreadingTCPServer(("127.0.0.1", 0), Echo) for _ in range(2)]
        for server in servers:
            server.daemon_threads = True
            threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            ports = [s.server_address[1] for s in servers]
            audit_key, audit_log = directory / "audit.key", directory / "audit.jsonl"
            audit_key.write_text("0123456789abcdef0123456789abcdef")
            os.chmod(audit_key, 0o600)
            allowed = (exec_mode.replace("127.0.0.1:9", f"127.0.0.1:{ports[0]}") +
                       "channel_invocation_id = run-42\nchannel_connect_timeout_ms = 120000\n"
                       f"audit_log = {audit_log}\naudit_key_file = {audit_key}\n"
                       "audit_key_id = test-key\n")
            config.write_text(allowed)

            # Policy applies, and receipts go to the audit log, not to stdout.
            assert run(config, PROBE, "connect", "127.0.0.1", ports[0]) == "echo ok\n"
            assert run(config, PROBE, "connect", "127.0.0.1", ports[1], status=3) == "refused denied\n"
            # The documented example is a program this command starts as is.
            assert run(config, EXAMPLE, "127.0.0.1", ports[0]) == (
                "client: the stream answered 26 bytes: hello through the channel\n")
            records = audit_log.read_text()
            assert "agent-01" in records and "run-42" in records
            assert all("mac" in json.loads(line) for line in records.splitlines())

            # SIGTERM and SIGINT to the launcher reach the program, and the
            # relayed connection still works afterwards: the server stops
            # only once the program has exited.
            for number in (signal.SIGTERM, signal.SIGINT):
                process = launch(config, PROBE, "hold", "127.0.0.1", ports[0])
                assert line(process, process.stdout) == "ready\n"
                process.send_signal(number)
                out, err = finish(process)
                assert (process.returncode, out, err) == (0, "echo after signal ok\n", ""), (out, err)

            # A program with default dispositions is ended by the signal, and
            # the status says so: 128 + signal.
            for number in (signal.SIGTERM, signal.SIGINT):
                process = launch(config, PROBE, "sleep")
                assert line(process, process.stdout) == "ready\n"
                process.send_signal(number)
                out, err = finish(process)
                assert (process.returncode, out, err) == (128 + number, "", ""), (out, err)

            # SIGHUP is the policy reload: it is not forwarded (the default
            # disposition would end the program), and its outcome is one
            # line on stderr. A refused reload changes nothing.
            process = launch(config, PROBE, "hold", "127.0.0.1", ports[0])
            assert line(process, process.stdout) == "ready\n"
            config.write_text(allowed.replace("agent-01", "agent-02"))
            process.send_signal(signal.SIGHUP)
            assert line(process, process.stderr).startswith(
                "maelys-egress: policy reload rejected: ")
            config.write_text(allowed.replace(f"127.0.0.1:{ports[0]}", f"127.0.0.1:{ports[1]}"))
            process.send_signal(signal.SIGHUP)
            assert line(process, process.stderr).startswith(
                "maelys-egress: policy reloaded: generation 2, sha256 ")
            process.send_signal(signal.SIGTERM)
            out, err = finish(process)
            assert (process.returncode, out, err) == (0, "echo after signal ok\n", ""), (out, err)
            config.write_text(allowed)

            # What the program leaves behind loses its connection when the
            # program exits, and the launcher does not wait for it nor for
            # channel_connect_timeout_ms: that is two minutes here and one
            # launch is given half of it, so a launcher that waited would be
            # reported as one that did not return. The proof of the
            # revocation is the note the process left behind writes when its
            # read ends.
            assert LIMIT * 1000 < 120000
            note = directory / "left-behind"
            assert run(config, PROBE, "leave", "127.0.0.1", ports[0], note, status=7) == ""
            for _ in range(LIMIT * 20):
                if note.exists() and note.read_text():
                    break
                time.sleep(0.05)
            assert note.exists() and note.read_text().startswith("revoked "), "nothing was revoked"
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()
    print("channel exec: contract, modes, refusals, status, descriptor, signals, reload and shutdown passed")


if __name__ == "__main__":
    try:
        test()
    finally:
        for group in GROUPS:
            end_group(group)
