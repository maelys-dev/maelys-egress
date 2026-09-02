# Getting started

This walkthrough explains the terms first, then starts Egress, sends an allowed
request, observes a denied request, and checks the daemon's health.

## What is a proxy, and where does SOCKS5 fit?

A **proxy** is an intermediary: the application connects to Egress, and Egress
connects to the destination only if policy permits it. “Proxy” is the role;
HTTP proxy and SOCKS5 are two different wire protocols for asking the proxy to
open that connection.

| Client protocol | What the client sends to Egress | Typical use |
|---|---|---|
| HTTP forward proxy | An HTTP request containing an absolute `http://` URL | Plain HTTP |
| HTTP `CONNECT` | “Open a TCP tunnel to `host:port`” | HTTPS; TLS remains end-to-end between the application and origin |
| SOCKS5 `CONNECT` | A binary request containing a host and port | Generic TCP clients, including HTTPS and non-HTTP protocols |

Egress supports all three on the same proxy listener. It does not translate an
application protocol into another one: after an HTTP `CONNECT` or SOCKS5
connection is admitted, it relays bytes in both directions.

## 1. Create private configuration

The proxy credential is deliberately kept outside the configuration file.

```sh
install -d -m 0700 "$HOME/.maelys-egress-demo"
openssl rand -base64 32 >"$HOME/.maelys-egress-demo/token"
chmod 0600 "$HOME/.maelys-egress-demo/token"

cat >"$HOME/.maelys-egress-demo/egress.conf" <<EOF
schema_version = 1
listen = 127.0.0.1:18080
admin_listen = 127.0.0.1:19901
token_file = $HOME/.maelys-egress-demo/token
allow_tls_sni = github.com:443
EOF
chmod 0600 "$HOME/.maelys-egress-demo/egress.conf"
```

This policy admits exactly `github.com:443`. `allow_tls_sni` also requires the
TLS ClientHello inside the tunnel to name `github.com`; a connection to another
virtual host on the same IP is therefore refused.

## 2. Validate and start

Validation parses the file, resolves the destination, seals its DNS addresses
and exits without opening a listener:

```sh
maelys-egress config validate --config "$HOME/.maelys-egress-demo/egress.conf"
```

Expected shape:

```text
Configuration is valid. Policy SHA-256: <64 hexadecimal characters>
```

Start Egress in terminal A and leave it running:

```sh
maelys-egress serve --config "$HOME/.maelys-egress-demo/egress.conf"
```

Its first stdout JSON event identifies the proxy port, policy digest and
operations port:

```json
{"schemaVersion":1,"contract":"maelys-egress-lifecycle/1","event":"ready","proxy":{"transport":"tcp","host":"127.0.0.1","port":18080},"admin":{"host":"127.0.0.1","port":19901},"policy":{"generation":1,"algorithm":"sha256","digest":"..."}}
```

Connection receipts and reload events continue as JSON Lines on stdout;
diagnostics use stderr. Process supervisors must keep stdout drained.

## 3. Send an HTTP-proxy request

In terminal B:

```sh
TOKEN=$(cat "$HOME/.maelys-egress-demo/token")

curl --proxy http://127.0.0.1:18080 \
  --proxy-user "maelys:${TOKEN}" \
  https://github.com/
```

Because the requested URL is HTTPS, curl uses HTTP `CONNECT` to ask Egress for a
tunnel to `github.com:443`. Egress authenticates the proxy request, checks the
destination and SNI, then relays the encrypted origin TLS stream. Egress does not
decrypt the GitHub response.

This request is denied because `example.com:443` is absent from the policy:

```sh
curl --verbose --proxy http://127.0.0.1:18080 \
  --proxy-user "maelys:${TOKEN}" \
  https://example.com/
```

`--verbose` is used here so the HTTP proxy status and denial point are visible.
These interactive commands place the demo credential in curl's process
arguments. For a long-running production client, prefer its protected proxy
configuration or secret-injection mechanism and ensure it does not log the
proxy URL.

## 4. Send the equivalent SOCKS5 request

SOCKS5 is useful when the client supports SOCKS but not HTTP proxying, or when
the tunneled TCP protocol is not HTTP. The authentication remains username
`maelys` plus the token:

```sh
curl --socks5-hostname "maelys:${TOKEN}@127.0.0.1:18080" \
  https://github.com/
```

The word `hostname` matters:

- `--socks5-hostname` sends the literal name `github.com` to Egress; Egress applies
  its exact hostname policy and performs the pinned DNS connection;
- `--socks5` makes curl resolve locally and usually sends only an IP address.
  That IP is not the configured identity `github.com`, so an exact hostname
  policy can reject it.

This is about where DNS is performed, not about TLS: HTTPS is still encrypted
end-to-end in either SOCKS mode.

## 5. Check health and metrics

The operations listener on port 19901 is separate from the proxy listener. It
cannot open an upstream connection; it only reports daemon state and aggregate
counters.

```sh
curl --silent --show-error --fail http://127.0.0.1:19901/healthz
```

Expected healthy response:

```json
{"status":"ok","policy_generation":1,"audit_healthy":true}
```

Here `--fail` tells curl to return a non-zero shell status for HTTP errors. It
does not change Egress. This makes the command useful in a service health check:
Egress answers `200` when healthy and `503` when durable audit has degraded.
`--silent --show-error` removes the progress meter while retaining errors.

Metrics are Prometheus text counters:

```sh
curl --silent --show-error --fail http://127.0.0.1:19901/metrics
```

For example, `maelys_egress_admissions_total` counts admitted streams,
`maelys_egress_denials_total` counts policy denials, and
`maelys_egress_policy_generation` identifies the active policy generation. The
metrics deliberately contain no destination names, credentials or invocation
IDs.

## 6. Stop

Return to terminal A and press Ctrl-C. Egress stops accepting, closes its active
connections, emits terminal receipts and exits.

Egress is a mediator, not by itself a sandbox. A workload with another ambient
network route can ignore the proxy. See
[Maelys Sandbox–Executor–Egress integration](maelys-integration.md) for the
non-bypassable topology.
