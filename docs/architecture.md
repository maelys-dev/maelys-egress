# Architecture

## Boundaries

Maelys System owns readiness, clocks, wakeup, descriptor and socket mechanics. Egress
owns protocol parsing, authentication, policy matching, connection state,
buffers, DNS pinning, backpressure and receipts. Executor and Sandbox are not
Egress core link dependencies; a separately built integration adapter may link
their public APIs without changing that boundary.

The server creates and runs its System loop on one owner thread. The data plane
and receipt callback remain on that thread. `server_stop`, sealed-policy
replacement, policy-generation reads and immutable metric snapshots are the
documented cross-thread control operations. Stop maps to the loop's idempotent
wakeup. Every connection slot carries a generation in its event token, so a
stale readiness event cannot be attributed to a reused slot or descriptor.

## Source map

Each file owns one feature so that an audit can be scoped to one concern at
a time. `src/server_internal.h` is the private contract between the server
files; nothing in it is public ABI.

| Feature | Files |
| --- | --- |
| protocol parsers | `src/http.c`, `src/socks.c`, `src/clienthello.c` |
| policy, configuration, principals | `src/policy.c`, `src/config.c`, `src/profile.c` |
| server lifecycle and reactor loop | `src/server.c` |
| listeners and private connector pair | `src/server_listener.c` |
| admission, handshake, connect, close | `src/server_connection.c` |
| relay data plane and TLS steps | `src/server_relay.c` |
| byte quotas | `src/server_quota.c` |
| receipts, canonical evidence, attestation, durable audit | `src/receipt.c`, `src/server_receipt.c`, `src/audit.c`, `src/attestor.c` |
| native connector sessions | `src/connector.c`, `src/server_connector.c` |
| health and metrics listener | `src/server_admin.c` |
| TLS provider seam | `src/tls.c`, `providers/` |
| CLI catalog and handlers (on `libmaelys_cli`) | `cli/main.c`, `cli/commands.c`, `cli/schemas/` |
| CLI configuration keys, file and secrets | `cli/config_catalog.c`, `cli/config_file.c`, `cli/secrets.c` |
| CLI serve, reload and lifecycle stream | `cli/serve.c`, `cli/reload.c`, `cli/output.c`, `cli/tls_listener.c` |

## Whole system

```text
                         control plane
 configuration file ──► seal policy ──► generation replacement
        SIGHUP                                  │
                                                │
 client                                        ▼
   │ HTTP CONNECT / HTTP forward / SOCKS5   maelys-egress
   ▼                                      ┌────────────────────┐
 listener ─► authenticate ─► authorize ─► │ pinned connector   │ ─► upstream
 embedder ─► native session ─────────────► │                    │
                                          │ bounded relay      │
                                          │ SNI guard          │
                                          └─────────┬──────────┘
                                                    │
                        ┌───────────────────────────┼──────────┐
                        ▼                           ▼          ▼
                 receipt callback            HMAC audit   attestor

 Separate loopback operations listener:  /healthz  /metrics
```

The operations listener has its own bounded slots and parser. It cannot become
a proxy connection, select a destination or consume data-plane connection
slots. Durable audit and attestation are receipt consumers; they never grant a
network permission.

## Connection states

```text
ACCEPTED
   |
   +-- HTTP headers ---- authentication ---- exact authority
   |
   +-- SOCKS greeting -- user/password ---- exact destination
   |
   +-- native connector -- retained principal -- exact destination
   v
POLICY MATCH
   |
   v
NONBLOCKING CONNECT (pinned addresses, one deadline)
   |
   v
RELAY (two bounded buffers, level-triggered backpressure)
   |
   v
HALF-CLOSE / RECEIPT / RELEASE
```

HTTP forward mode deliberately handles one request per client connection and
forces `Connection: close`. This avoids cross-request authority changes and
keeps request routing checkable. `CONNECT` and SOCKS5 become byte tunnels after
the connection succeeds.

## Embedded lifecycle

```text
policy_create ─► allow_tcp ─► optional require_tls_sni ─► policy_seal
                                                               │
config_create ─► listener/auth/limits/receipts/audit ──────────┤
                                                               ▼
                                                        server_create
                                                               │
                                           owner thread ─► server_run
                                                               │
       another thread: connector_session_open / transfer fd ───┤
                                                               │
       another thread: metrics_snapshot / replace_policy / server_stop
                                                               │
                                                        server_destroy
```

For a library-only deployment, `config_set_native_only(1)` removes the proxy
listener from this lifecycle: the owner reactor, connectors, relays, receipts
and optional loopback operations listener remain, but no data-plane TCP port or
AF_UNIX pathname is exposed. `server_is_running()` lets the host establish the
readiness edge before opener threads call `connector_session_open()`.

Policies and configurations are copied by `server_create`; callers retain and
destroy their own handles. A replacement policy must already be sealed. An
active connection retains the destination, pinned addresses, digest and policy
generation admitted at its start, so a reload cannot rewrite history.

## Executor integration

Executor 0.13.0's existing fd 4 socket broker is a Unix datagram control
channel. It currently checks Executor's copied destination list, opens the
upstream TCP socket and returns that socket with `SCM_RIGHTS`. In that current
form Egress is not involved. Ordinary HTTP stacks instead reach Egress through a
standard HTTP/SOCKS proxy endpoint.

The target integration retains two frontends but not two enforcement engines:

```text
Executor confinement
  |-- fd 4 broker -------- Maelys-aware client ---------+
  `-- Egress proxy endpoint - standard HTTP/SOCKS client -+--> one sealed Egress
                                                            policy/connector
```

The fd 4 adapter must return the client side of a Egress-held relay stream, not
the unobserved upstream socket, if it claims the same SNI guard, byte accounting
and close receipts as the proxy frontend. `maelys_egress_profile` produces the
authenticated per-execution environment and matching receipt correlation
identity. The adapter copies those entries into a mutable execution request
and proves that the selected backend makes the proxy reachable without
restoring an ambient bypass; environment variables alone are never treated as
confinement. The complete topology and repository ownership are specified in
[network-mediation-design.md](network-mediation-design.md).

Egress's native connector now supplies that relay stream without knowing the fd-4

`native_only` is an embedding mode, not a reduced enforcement mode. It removes
only the externally reachable HTTP/SOCKS listener; connector traffic still
passes through authentication, the sealed policy, pinned connection setup,
guards, relay accounting and receipts.
wire protocol. It creates a private loopback TCP pair, admits and connects the
destination on the server's owner thread, returns only the client end and keeps
the peer in the normal guarded relay. The optional Executor-Egress adapter is
therefore a translation layer: fd-4 request in, `connector_session_open()` call,
returned TCP fd out through `SCM_RIGHTS`. It is not another policy engine.
