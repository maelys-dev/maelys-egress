#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Keep native leases, streams and a partial handshake alive during CLI exit."""
import array
import json
import os
from pathlib import Path
import signal
import socket
import struct
import tempfile

from test_broker_cli import Daemon, SCHEMA, validate


def receive_frame(peer, expected, descriptor_type=None):
    """Test peer: consume control on every fragment and close rights on failure."""
    data, descriptors = b"", []
    try:
        while len(data) < len(expected):
            block, ancillary, flags, _ = peer.recvmsg(len(expected) - len(data), 65536)
            unexpected = False
            for level, kind, payload in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    numbers = array.array("i")
                    size = numbers.itemsize
                    numbers.frombytes(payload[:len(payload) // size * size])
                    descriptors.extend(numbers)
                    unexpected |= len(payload) % size != 0
                else:
                    unexpected = True
            assert not flags and not unexpected and block, (block, flags, ancillary)
            data += block
            if peer.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) == socket.SOCK_DGRAM:
                break
        assert data == expected, (data, expected)
        assert len(descriptors) == int(descriptor_type is not None), descriptors
        if descriptor_type is not None:
            fd = descriptors[0]
            os.set_inheritable(fd, False)
            result = socket.socket(fileno=fd)
            descriptors.pop()
            try:
                assert result.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) == descriptor_type
                result.settimeout(3)
                return result
            except BaseException:
                result.close()
                raise
    finally:
        for fd in descriptors:
            os.close(fd)


def receive_exact(peer, length):
    data = b""
    while len(data) < length:
        block = peer.recv(length - len(data))
        assert block, (len(data), length)
        data += block
    return data


def run():
    # Resolve /tmp on macOS: the broker refuses symbolic path components.
    with tempfile.TemporaryDirectory(prefix="egress-stop-", dir="/tmp") as temporary:
        directory = Path(temporary).resolve()
        os.chmod(directory, 0o700)
        config, path = directory / "config", directory / "channel"
        with socket.socket() as upstream:
            upstream.bind(("127.0.0.1", 0))
            upstream.listen(8)
            upstream.settimeout(3)
            port = upstream.getsockname()[1]
            config.write_text(
                f"schema_version = 1\nchannel_listen_unix = {path}\n"
                "channel_principal = active-shutdown\nchannel_invocation_id = stop-42\n"
                f"allow_private = 127.0.0.1:{port}\n"
                "channel_handshake_timeout_ms = 60000\nchannel_max_clients = 5\n")
            for iteration in range(21):
                peers, closing_peers = [], []
                daemon = Daemon(config)
                failure = iteration == 20
                try:
                    ready = daemon.event("ready")
                    assert len(daemon.events) == 1 and "proxy" not in ready

                    def lease():
                        peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        peers.append(peer)
                        closing_peers.append(peer)
                        peer.settimeout(3)
                        peer.connect(str(path))
                        return peer

                    payload = b"kept-alive"
                    for _ in range(4):
                        bootstrap = lease()
                        bootstrap.sendall(b"MEBQ\x01\x01\x00\x00")
                        channel = receive_frame(bootstrap,
                            b"MEBP\x01\x00\x01\x00" + struct.pack("!Q", 5000), socket.SOCK_DGRAM)
                        peers.append(channel)
                        host = b"127.0.0.1"
                        request = b"MECQ\x01\x01" + struct.pack("!HH", port, len(host)) + host
                        assert channel.send(request) == len(request)
                        stream = receive_frame(channel, b"MECP\x01\x00\x00\x00", socket.SOCK_STREAM)
                        peers.append(stream)
                        closing_peers.append(stream)
                        accepted, _ = upstream.accept()
                        peers.append(accepted)
                        closing_peers.append(accepted)
                        accepted.settimeout(3)
                        stream.sendall(payload)
                        assert receive_exact(accepted, len(payload)) == payload
                        accepted.sendall(payload)
                        assert receive_exact(stream, len(payload)) == payload

                    slow = lease()
                    slow.sendall(b"M")
                    # Four live leases plus the incomplete handshake fill all
                    # five slots. BUSY proves the broker has accepted slow;
                    # a sleep would leave that boundary scheduler-dependent.
                    probe = lease()
                    receive_frame(probe, b"MEBP\x01\x02\x01\x00" + bytes(8))
                    if failure:
                        os.chmod(directory, 0o750)
                        lease()  # Accept detects loss of the trusted parent.
                    else:
                        daemon.process.send_signal(signal.SIGINT if iteration % 2 else signal.SIGTERM)
                    assert daemon.process.wait(timeout=5) == int(failure), iteration
                    daemon.reader.join(timeout=2)
                    assert not daemon.reader.is_alive()
                    assert not daemon.process.stderr.read()
                    while True:
                        line = daemon.lines.get_nowait()
                        if line is None:
                            break
                        event = json.loads(line)
                        assert not validate(event, SCHEMA), event
                        daemon.events.append(event)
                    names = [event["event"] for event in daemon.events]
                    assert names[0] == "ready" and names.count("ready") == 1, names
                    if failure:
                        assert names.count("fatal") == 1 and "stopped" not in names, names
                        # Unsafe-parent cleanup must retain the socket inode.
                        assert path.is_socket()
                    else:
                        assert "fatal" not in names and names.count("stopping") == 1, names
                        assert names.count("stopped") == 1 and names[-1] == "stopped", names
                        assert not path.exists()
                    receipts = [e["receipt"] for e in daemon.events if e["event"] == "receipt"]
                    assert len(receipts) == 4 and len({r["id"] for r in receipts}) == 4, receipts
                    for receipt in receipts:
                        assert receipt["principal"] == "active-shutdown", receipt
                        assert receipt["invocationId"] == "stop-42", receipt
                        assert receipt["bytesFromClient"] == receipt["bytesToClient"] == len(payload), receipt
                    for peer in closing_peers:
                        try:
                            # Unix peers can carry ancillary data: never silently
                            # discard it via recv, including on this EOF check.
                            block, ancillary, flags, _ = peer.recvmsg(1, 65536)
                            rights = []
                            for level, kind, raw in ancillary:
                                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                                    numbers = array.array("i")
                                    numbers.frombytes(raw[:len(raw) // numbers.itemsize * numbers.itemsize])
                                    rights.extend(numbers)
                            for fd in rights:
                                os.close(fd)
                            assert not block and not ancillary and not flags
                        except ConnectionResetError:
                            pass  # Linux can reset a peer with unread bytes.
                finally:
                    daemon.close()
                    for peer in peers:
                        peer.close()
                    os.chmod(directory, 0o700)
                    if path.exists():
                        path.unlink()
    print("broker shutdown: 20 signal stops and 1 failure passed with 4 live leases/streams and a partial handshake")


if __name__ == "__main__":
    run()
