#!/usr/bin/env python3
"""Write the seed corpus of the three fuzz targets.

A seed is a valid or nearly valid input: it carries the structure the parser
looks for, so a campaign spends its budget on the boundaries instead of
rediscovering that a request starts with a method or that a TLS record starts
with a handshake byte. Run from the repository root after changing a parser's
accepted shape; the files it writes are committed.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent


def write(target: str, name: str, data: bytes) -> None:
    path = ROOT / target / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


AUTH = b"Proxy-Authorization: Bearer 0123456789abcdef\r\n"

HTTP = {
    "connect-authenticated":
        b"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n" + AUTH + b"\r\n",
    "connect-basic-credential":
        b"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n"
        b"Proxy-Authorization: Basic bWFlbHlzOjAxMjM0NTY3ODlhYmNkZWY=\r\n\r\n",
    "connect-bracketed-ipv6":
        b"CONNECT [2001:db8::1]:443 HTTP/1.1\r\nHost: [2001:db8::1]:443\r\n" + AUTH + b"\r\n",
    "forward-get":
        b"GET http://example.com/path HTTP/1.1\r\nHost: example.com\r\n" + AUTH + b"\r\n",
    "forward-query-without-path":
        b"GET http://example.com?x=1 HTTP/1.1\r\nHost: example.com\r\n" + AUTH + b"\r\n",
    "forward-post-content-length":
        b"POST http://example.com/x HTTP/1.1\r\nHost: example.com\r\n" + AUTH +
        b"Content-Length: 3\r\n\r\n",
    "forward-htab-in-value":
        b"GET http://example.com/x HTTP/1.1\r\nHost: example.com\r\n" + AUTH +
        b"X-Note: a\tb\r\n\r\n",
    # Refused shapes: the parser must reject them, and they sit one byte away
    # from the accepted ones, which is where a mutation is worth spending.
    "refused-bare-cr":
        b"GET http://example.com/x HTTP/1.1\r\nHost: example.com\r\n" + AUTH +
        b"X-Ambiguous: one\rInjected: two\r\n\r\n",
    "refused-duplicate-content-length":
        b"POST http://example.com/x HTTP/1.1\r\nHost: example.com\r\n" + AUTH +
        b"Content-Length: 1\r\nContent-Length: 2\r\n\r\n",
    "refused-transfer-encoding":
        b"POST http://example.com/x HTTP/1.1\r\nHost: example.com\r\n" + AUTH +
        b"Transfer-Encoding: chunked\r\n\r\n",
    "refused-authority-mismatch":
        b"CONNECT example.com:443 HTTP/1.1\r\nHost: attacker.example:443\r\n" + AUTH + b"\r\n",
}

SOCKS = {
    "greeting-no-authentication": bytes([5, 1, 0]),
    "greeting-username-password": bytes([5, 1, 2]),
    "authentication-username-password":
        bytes([1, 6]) + b"maelys" + bytes([16]) + b"0123456789abcdef",
    "request-domain": bytes([5, 1, 0, 3, 11]) + b"example.com" + bytes([1, 187]),
    "request-ipv4": bytes([5, 1, 0, 1, 127, 0, 0, 1, 0, 9]),
    "request-ipv6": bytes([5, 1, 0, 4]) + bytes([0x20, 0x01, 0x0d, 0xb8] + [0] * 11 + [1])
        + bytes([1, 187]),
    "refused-domain-embedded-nul":
        bytes([5, 1, 0, 3, 11]) + b"exam\x00le.com" + bytes([1, 187]),
}


def client_hello(names: list[bytes], record_bytes: int | None = None) -> bytes:
    entries = b"".join(bytes([0]) + len(name).to_bytes(2, "big") + name for name in names)
    extension = len(entries).to_bytes(2, "big") + entries
    extension_block = bytes([0, 0]) + len(extension).to_bytes(2, "big") + extension
    body = (bytes([3, 3]) + bytes([0x5a] * 32) + bytes([0])
            + (2).to_bytes(2, "big") + bytes([0x13, 0x01])
            + bytes([1, 0])
            + len(extension_block).to_bytes(2, "big") + extension_block)
    handshake = bytes([1]) + len(body).to_bytes(3, "big") + body
    record = bytes([22, 3, 3]) + len(handshake).to_bytes(2, "big") + handshake
    return record[:record_bytes] if record_bytes is not None else record


CLIENTHELLO = {
    "valid-single-name": client_hello([b"example.com"]),
    "refused-two-names": client_hello([b"example.com", b"other.example"]),
    "refused-embedded-nul": client_hello([b"example.com\x00.other.example"]),
    "refused-empty-name": client_hello([b""]),
    "truncated-record": client_hello([b"example.com"], record_bytes=9),
}


def main() -> int:
    for name, data in HTTP.items():
        write("http", name, data)
    for name, data in SOCKS.items():
        write("socks", name, data)
    for name, data in CLIENTHELLO.items():
        write("clienthello", name, data)
    written = sum(len(group) for group in (HTTP, SOCKS, CLIENTHELLO))
    print(f"corpus: {written} seeds written under {ROOT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
