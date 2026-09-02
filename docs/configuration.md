# Standalone configuration

For a command-by-command deployment, including creation of the token, audit
key and output journal, start with [standalone operation](standalone-guide.md).
This page is the exhaustive key reference.

The daemon accepts one strict configuration file. Operational listener, policy,
authentication, quota, audit and TLS flags deliberately do not exist on
`serve`: silently merging two sources makes review and reload ambiguous.

```sh
maelys-egress config validate --config /etc/maelys-egress.conf
maelys-egress serve --config /etc/maelys-egress.conf
```

The file is line-oriented `key = value`. Blank lines and lines beginning with
`#` are ignored. There is no quoting, interpolation, include, environment
expansion or inline secret. Unknown keys, duplicate scalar keys, embedded NULs
and lines over 4096 bytes are rejected. The file must be a non-symlink regular
file owned by root or the daemon user and must not be group/world writable.

Supported keys are:

```text
schema_version = 1
listen = 127.0.0.1:8080
listen_unix = /run/maelys-egress/egress.sock
unix_peer = authenticated | same-euid
token_file = /run/secrets/maelys-egress-token
unauthenticated_loopback = true | false
max_connections = 128
quota_connections = 8
quota_bytes = 104857600
quota_total_bytes = 1073741824
admin_listen = 127.0.0.1:9901
audit_log = /var/log/maelys-egress/audit.jsonl
audit_key_file = /run/secrets/maelys-egress-audit-key
audit_key_id = host-2026-08
allow = example.com:443
allow_private = 127.0.0.1:8080
allow_tls_sni = example.com:443
tls_cert = /path/to/certificate.pem
tls_key = /path/to/private-key.pem
tls_ca = /path/to/client-ca.pem
require_client_cert = true | false
```

`schema_version = 1` is mandatory, so a future grammar is never guessed. Only
the three `allow*` keys are repeatable. `listen` and `listen_unix` are
mutually exclusive. `unix_peer` is valid only with `listen_unix`. Unix
listeners always require the proxy credential from `token_file`; same-EUID is
an additional peer check, not an authentication substitute.

This statement describes the standalone configuration format. Embedders can
instead call `maelys_egress_config_set_unix_principal()` on a private
`same-euid` AF_UNIX listener. That opt-in library mode binds the listener to one
principal and deliberately accepts only credential-free HTTP/SOCKS requests;
it is not exposed as a standalone unauthenticated-listener switch.

The token file remains separate, owner-only mode 0600 and at least 16 bytes.
This makes the main configuration safe to distribute without embedding its
credential.

`quota_connections` limits concurrent admitted streams for the `maelys`
principal configured by the standalone CLI. `quota_bytes` limits the sum of
admitted relay-payload bytes observed from and to one client connection. Proxy
authentication and HTTP/SOCKS framing are excluded. `quota_total_bytes` limits
the same payload cumulatively across all sequential and concurrent streams of
that principal. Zero means unlimited for either byte ceiling. Egress caps each
I/O request to the remaining budget rather than detecting an overshoot after
the transfer. The library API can assign different limits to each of its up to
64 principals.

`admin_listen` must be numeric loopback. It exposes only aggregate data at
`GET /healthz` and `GET /metrics`; it never shares proxy connection slots or
the proxy protocol parser. Port zero is useful for tests, while services should
use a stable port.

The three audit keys are all-or-none. The HMAC key file must be owner-only and
16–4096 bytes. The audit log is an exclusively locked, owner-only regular file.
Each receipt is appended as one JSON line and `fdatasync` completes before the
next receipt is accepted as durable. Startup verifies the complete existing
sequence, key id, previous-MAC link and HMAC before appending. This detects
modification, reordering and partial writes. It does not prove facts to a party
without the shared key, and deletion of complete records at the tail requires
an externally stored final-chain checkpoint to detect.

`audit_log` is an output path, `audit_key_file` is an operator-generated shared
secret, and `audit_key_id` is an operator-chosen public rotation label. They are
not values obtained from Egress. Concrete creation and inspection commands are
in [durable audit](standalone-guide.md#durable-audit-create-it-and-read-it).

The four TLS/client-certificate keys are understood only by the
provider-specific `maelys-egress-mbedtls` and `maelys-egress-wolfssl` binaries. See
[TLS deployment](tls.md) for build, certificate, curl and mTLS examples.

## Reload

Sending `SIGHUP` to a process started with `serve --config` rebuilds and seals a new
policy, then atomically installs it for future admissions:

```sh
kill -HUP "$(cat /run/maelys-egress/egress.pid)"
```

Only `allow`, `allow_private` and `allow_tls_sni` may change. Any change to the
listener, authentication, quotas, TLS, audit or resource limits rejects the
reload and leaves the active generation untouched. Already admitted streams
continue with their copied destination and original digest; their receipts
therefore remain truthful.

The systemd example creates `/run/maelys-egress` mode 0700. For launchd, create a
dedicated directory such as `/var/db/maelys-egress/run`, owned by
`_maelys-egress`, mode 0700, and use a socket below it. The container example
expects the configuration and token to be mounted at runtime; neither is baked
into the image.

The exhaustive table is generated from the executable catalog in
[the configuration-key reference](generated/config-reference.md). Agents can
retrieve the same contract directly with:

```sh
maelys-egress config describe --format json --compact --non-interactive
```
