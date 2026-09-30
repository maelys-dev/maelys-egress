#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Local TCP fixture: report the actual destination IP over the mediated stream."""
import selectors
import socket

selector = selectors.DefaultSelector()
for port in (8080, 8081):
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", port))
    listener.listen(16)
    listener.setblocking(False)
    selector.register(listener, selectors.EVENT_READ, None)

print("upstream: both policy-test ports are listening", flush=True)
while True:
    for key, _ in selector.select():
        peer = key.fileobj
        if key.data is None:
            stream, _ = peer.accept()
            stream.setblocking(False)
            selector.register(stream, selectors.EVENT_READ, bytearray())
            continue
        block = peer.recv(128)
        if not block:
            selector.unregister(peer)
            peer.close()
            continue
        key.data.extend(block)
        if len(key.data) > 128:
            selector.unregister(peer)
            peer.close()
        elif key.data.endswith(b"\n"):
            if key.data != b"native-compose\n":
                raise RuntimeError("unexpected fixture request")
            # Small bounded reply; the example only has one request in flight.
            peer.settimeout(2)
            peer.sendall(f"native-compose {peer.getsockname()[0]}\n".encode("ascii"))
            peer.setblocking(False)
            key.data.clear()
