# Troubleshooting

Egress fails closed: when it cannot prove configuration safety, authenticate a
client, match the exact destination or preserve audit integrity, it refuses the
operation. Diagnose the stage in this order:

```text
configuration ─► listener ─► authentication ─► policy ─► upstream ─► relay/audit
```

## First three commands

```sh
# 1. Parse, resolve and seal without opening a listener.
maelys-egress config validate --config /etc/maelys-egress.conf

# 2. Show the proxy exchange, including HTTP status.
curl --verbose --proxy http://127.0.0.1:8080 \
  --proxy-user "maelys:$(cat /run/secrets/maelys-egress-token)" \
  https://github.com/

# 3. Show daemon state even when health returns HTTP 503.
curl --silent --show-error --include http://127.0.0.1:9901/healthz
```

Inspect Egress stderr for setup diagnostics and stdout for the structured
lifecycle, reload and receipt stream. Under systemd:

```sh
journalctl -u maelys-egress --since '10 minutes ago'
```

## Configuration is rejected

Expected success:

```text
Configuration is valid. Policy SHA-256: <64 hexadecimal characters>
```

If it fails, check:

```sh
ls -ld /etc /etc/maelys-egress.conf /run/secrets/maelys-egress-token
```

- Configuration, token and audit key must be regular non-symlink files owned
  by root or the daemon user and not writable by group/world. Use mode 0600.
- The token must contain 16–255 visible ASCII bytes. Generate one with
  `openssl rand -base64 32`; do not use an empty placeholder.
- Only `allow`, `allow_private` and `allow_tls_sni` may repeat. Scalar keys such
  as `listen` or `max_connections` may appear once.
- `schema_version = 1` is required. `serve` has no command-line policy source.
- Unknown keys and inline comments are errors. Comments occupy their own line.

If sealing reports a private/non-global address, either the destination is
unexpectedly private or the policy must explicitly use:

```text
allow_private = 127.0.0.1:8080
```

Do not use `allow_private` merely to silence an unexpected DNS result; verify
the name and resolver first.

## Listener does not start

For TCP, verify that the numeric address exists locally and the port is free:

```sh
# Linux
ss -ltn | grep ':8080 '

# macOS
lsof -nP -iTCP:8080 -sTCP:LISTEN
```

For AF_UNIX:

```sh
ls -ld /run/maelys-egress
ls -l /run/maelys-egress/egress.sock
```

The parent must already exist, be owned by the daemon user and have mode 0700.
The socket path itself must be absent before startup. A stale node should be
investigated first: confirm no Egress process is using it before moving it out of
the way.

A plaintext non-loopback TCP listener is deliberately refused. Use a
provider-specific TLS binary and certificate as shown in
[TLS deployment](tls.md).

## HTTP status 407: authentication failed

`407 Proxy Authentication Required` means the request reached the proxy parser
but no configured principal matched.

For the standalone CLI, the username is always `maelys` and the secret is the
exact token-file content:

```sh
TOKEN=$(cat /run/secrets/maelys-egress-token)
curl --verbose --proxy http://127.0.0.1:8080 \
  --proxy-user "maelys:${TOKEN}" https://github.com/
```

Common mistakes are using the destination's `Authorization` header instead of
proxy authentication, using another username, reading another token file, or
pointing the client at the operations port rather than the proxy port.

For a Bearer test:

```sh
curl --verbose --proxy http://127.0.0.1:8080 \
  --proxy-header "Proxy-Authorization: Bearer ${TOKEN}" \
  https://github.com/
```

## HTTP status 403: policy or quota denied

`403 Forbidden` means authentication succeeded but Egress refused admission.
Check the exact spelling and port in the request and configuration:

```text
requested: github.com:443
configured: allow_tls_sni = github.com:443
```

There are no wildcard or suffix rules. `api.github.com` and `github.com` are
different identities. A principal at its active-connection quota also receives
a denial; inspect:

```sh
curl --silent http://127.0.0.1:9901/metrics |
  grep -E 'denials_total|quota_denials_total'
```

With `allow_tls_sni`, HTTP `CONNECT` may first succeed and the tunnel may then
close during the origin TLS handshake if the ClientHello has no readable exact
SNI, names another host or contains ECH. The terminal receipt on stderr records
the denial.

## HTTP status 400 or 502

- `400 Bad Request` means Egress rejected proxy framing: oversized/ambiguous
  headers, malformed authority, unsupported HTTP version, or an unsupported
  transfer-encoded forward request. Use `curl --verbose` and ensure the client
  is configured for a proxy rather than sending an origin-form request by hand.
- `502 Bad Gateway` means the request was admitted but none of the policy's
  pinned addresses could be connected before the deadline. Check origin
  availability, host firewall and routing from the Egress process.

## SOCKS5 request is denied

Use remote-name mode for hostname policies:

```sh
curl --verbose \
  --socks5-hostname "maelys:${TOKEN}@127.0.0.1:8080" \
  https://github.com/
```

`--socks5` without `-hostname` makes curl resolve locally and send an IP. Egress
then sees that IP rather than `github.com`, so it cannot match the configured
hostname identity. SOCKS5 supports only the `CONNECT` command in Egress 0.7; UDP
associate and bind are unsupported.

## Listener TLS fails

First verify that the selected executable contains a TLS provider:

```sh
maelys-egress-mbedtls --help | grep tls-cert
maelys-egress-wolfssl --help | grep tls-cert
```

The minimal `maelys-egress` binary intentionally has no TLS options. Then inspect
the listener handshake separately from the origin:

```sh
curl --verbose --proxy https://127.0.0.1:18443 \
  --proxy-cacert proxy-cert.pem \
  --proxy-user "maelys:${TOKEN}" \
  https://github.com/
```

- An error before `CONNECT` concerns Egress's listener certificate, CA, name or
  optional client certificate.
- An error after `200 Connection Established` concerns the origin tunnel, SNI,
  ECH or origin certificate.

The certificate SAN must contain the name or IP used in the proxy URL. For
mTLS, also pass `--proxy-cert` and `--proxy-key`; `--cert` without `proxy-`
selects the origin client certificate instead.

## ECH and exact SNI

Egress can inspect a normal TLS ClientHello SNI without terminating origin TLS.
With ECH, the true inner SNI is encrypted to the destination; Egress does not own
that key. An `allow_tls_sni` policy therefore refuses ECH rather than treating
the public outer name as proof. There is no transparent configuration fix that
both preserves ECH and proves the inner name without TLS termination. See the
postures in [network mediation design](network-mediation-design.md).

## Policy reload did not apply

After atomically replacing the file and sending `SIGHUP`, check both stderr and
the generation:

```sh
kill -HUP "$EGRESS_PID"
curl --silent http://127.0.0.1:9901/metrics |
  grep maelys_egress_policy_generation
```

Only the three destination keys may change. A listener, token, quota, TLS,
audit or limit change rejects reload and leaves the old generation active.
`maelys_egress_policy_reload_failures_total` counts these failures.

## Durable audit prevents startup or health is degraded

Check ownership, permissions and exclusivity:

```sh
ls -ld /var/lib/maelys-egress/audit
ls -l /var/lib/maelys-egress/audit/hmac.key \
      /var/lib/maelys-egress/audit/receipts.jsonl
```

- Key and log must belong to the effective daemon user and be mode 0600.
- Only one process may hold the log.
- The same `audit_key_id` and key must verify the existing chain after restart.
- Editing, truncating, reordering or partially writing the JSONL file is a hard
  integrity failure, not something Egress repairs automatically.

To see a degraded response and its cause counters without hiding HTTP 503:

```sh
curl --silent --show-error --include http://127.0.0.1:9901/healthz
curl --silent http://127.0.0.1:9901/metrics |
  grep -E 'audit_failures|reload_failures'
```

Repair the storage or attestor first, then restart with the original verified
journal and correct key. Do not discard the log merely to turn health green.

## Workload can bypass Egress

Proxy variables are cooperation, not confinement. If this succeeds from a
supposedly mediated workload, the sandbox topology is wrong:

```sh
curl --noproxy '*' https://example.com/
```

Bubblewrap must retain a private network namespace and expose only the AF_UNIX
bridge; Seatbelt must deny ambient network except for a proven mediator route.
The POSIX backend cannot provide this guarantee. See
[Sandbox–Executor–Egress integration](maelys-integration.md).
