# Standalone operation

The standalone product is the `maelys-egress` daemon plus one strict
configuration file. Applications keep using standard HTTP or SOCKS clients;
they do not need a Maelys API.

```text
application ── HTTP proxy or SOCKS5 ──► maelys-egress ──► allowed TCP origin
                                            │
                                  health / metrics / receipts
```

## Choosing a listener

The listener answers a deployment question: **how does the client reach
Egress?** It does not change destination policy.

### Local TCP: easiest for a host application

```text
application on this host ── 127.0.0.1:8080 ──► Egress
```

Configuration:

```text
schema_version = 1
listen = 127.0.0.1:8080
token_file = /run/user/1000/maelys-egress/token
allow = example.com:443
```

Use this when both processes run on the same host and the application already
supports `HTTP_PROXY`, `HTTPS_PROXY`, `ALL_PROXY`, `--proxy` or SOCKS5. Other
local processes can reach the port, so the token remains mandatory in
production.

### Filesystem AF_UNIX: sidecar or sandbox bridge

```text
sandbox/sidecar relay ── /run/maelys-egress/egress.sock ──► Egress
```

Create the private parent before starting Egress:

```sh
sudo install -d -o maelys-egress -g maelys-egress -m 0700 /run/maelys-egress
```

Configuration:

```text
schema_version = 1
listen_unix = /run/maelys-egress/egress.sock
unix_peer = same-euid
token_file = /run/secrets/maelys-egress-token
allow_tls_sni = github.com:443
```

`same-euid` requires the connecting Unix peer to have Egress's effective user ID
in addition to knowing the proxy token. `authenticated` checks only the token.
The socket path must not exist before startup; Egress creates it and removes only
that exact socket node during clean shutdown.

Most ordinary applications expect a TCP proxy address rather than an AF_UNIX
path. In a Bubblewrap deployment, Executor therefore mounts only this socket
into the private namespace and runs a small protocol-blind loopback relay
inside it. The workload reaches `127.0.0.1:<ephemeral>`; the relay transports
the same HTTP/SOCKS bytes to the Unix socket. Egress remains the only component
that authenticates and authorizes the destination.

### Remote TCP: protect the client-to-Egress hop with TLS

```text
remote client ══ TLS ══► Egress TLS listener ──► allowed origin
```

A non-loopback listener is accepted only by
`maelys-egress-mbedtls` or `maelys-egress-wolfssl` with a certificate and key. TLS
prevents observers on that network hop from seeing the proxy credential. Proxy
authentication is still mandatory, and optional mutual TLS can also identify
the client machine. See [TLS deployment](tls.md) for complete commands.

## Authentication

With the standalone CLI, `token_file` creates exactly one principal:

```text
username = maelys
password/bearer token = contents of token_file
```

The same identity is carried differently by each client protocol:

| Protocol | Credential on the wire |
|---|---|
| HTTP proxy | Basic `maelys:TOKEN`, or Bearer `TOKEN` |
| SOCKS5 | Username `maelys`, password `TOKEN` |

On a plaintext listener those credentials are plaintext at the transport
layer. Plain TCP is therefore restricted to numeric loopback. AF_UNIX relies on
local filesystem isolation; remote TCP requires listener TLS.

## Reload without interrupting existing streams

Only destination entries may be reloaded. Build a complete replacement file,
make it private, atomically rename it, then send `SIGHUP`:

```sh
install -m 0600 /tmp/egress.conf.new /etc/maelys-egress.conf.new
mv /etc/maelys-egress.conf.new /etc/maelys-egress.conf
kill -HUP "$EGRESS_PID"
```

Egress parses, resolves and seals the entire candidate before installing it.
When successful, stdout emits a `policy-reloaded` lifecycle event and
`/metrics` exposes generation 2. A malformed candidate emits
`policy-reload-rejected` and leaves generation 1
active. Existing streams retain the policy digest, DNS pins and generation
under which they were admitted.

Listener, authentication, quotas, TLS, audit and resource limits require a
restart; changing them during `SIGHUP` is refused.

## Durable audit: create it and read it

The standalone lifecycle writes live receipt events to stdout. Durable audit adds an
owner-only append-only journal whose records are linked with HMAC-SHA-256. It
is useful when a connection history must survive restart or feed an incident
and compliance pipeline.

The three configuration values come from the operator:

- `audit_log` is the output JSONL file Egress creates or resumes;
- `audit_key_file` is a secret HMAC key that **you generate**;
- `audit_key_id` is a non-secret label **you choose** so key rotations can be
  identified, for example `host-2026-08`.

Create the directory and key:

```sh
sudo install -d -o maelys-egress -g maelys-egress -m 0700 \
  /var/lib/maelys-egress/audit
sudo openssl rand -out /var/lib/maelys-egress/audit/hmac.key 32
sudo chown maelys-egress:maelys-egress \
  /var/lib/maelys-egress/audit/hmac.key
sudo chmod 0600 /var/lib/maelys-egress/audit/hmac.key
```

Do not pre-create the log as another user. Add all three keys:

```text
audit_log = /var/lib/maelys-egress/audit/receipts.jsonl
audit_key_file = /var/lib/maelys-egress/audit/hmac.key
audit_key_id = host-2026-08
```

After connections finish:

```sh
sudo -u maelys-egress tail -n 1 \
  /var/lib/maelys-egress/audit/receipts.jsonl | jq .
```

Each line contains a sequence number, key ID, previous MAC, current MAC and the
canonical connection receipt: principal, destination, outcome, byte counts,
policy digest and generation. At restart Egress reads and verifies the complete
chain before accepting new records. A wrong key, altered line, reordered line,
partial tail, unsafe permissions or another process holding the file lock makes
startup fail closed.

Anyone with the HMAC key can forge records; this is shared-key integrity, not
an independent signature. To detect deletion of complete records from the end,
periodically copy the last record's `mac` to a separate trusted system:

```sh
tail -n 1 /var/lib/maelys-egress/audit/receipts.jsonl |
  jq -r .mac | your-remote-checkpoint-command
```

The C embedding API can instead install an Ed25519, HSM or platform-keystore
attestor. Egress supplies the provider seam but deliberately does not choose a
signing stack; `examples/custom_attestor.c` demonstrates the ABI with an
explicitly non-secure placeholder signer.

## Health and metrics in service management

Configure a stable local port:

```text
admin_listen = 127.0.0.1:9901
```

Use this as a health command:

```sh
curl --silent --show-error --fail http://127.0.0.1:9901/healthz >/dev/null
```

The command exits 0 for HTTP 200 and non-zero for degraded HTTP 503. A metrics
collector can scrape `http://127.0.0.1:9901/metrics`. This listener exposes
aggregates only and cannot relay traffic.

## Shutdown

- In a foreground terminal, press Ctrl-C (`SIGINT`).
- For a manually backgrounded daemon, run `kill -TERM "$EGRESS_PID"`.
- With systemd, run `systemctl stop maelys-egress`.
- With launchd, unload or stop the configured service.

`SIGINT` and `SIGTERM` request the same bounded shutdown: stop admitting new
clients, close active streams, emit their terminal receipts, flush durable
audit and release the listener. `SIGHUP` is different: it requests only a
policy reload.

See [configuration reference](configuration.md) for every key and
[troubleshooting](troubleshooting.md) for failure diagnosis.
