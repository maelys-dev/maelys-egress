# Embedding the C library

`libmaelys-egress` is the native embedding API. Its ABI 2 surface uses opaque
handles and owned errors. It links to `libmaelys-sys`; no concrete TLS library
is required for a plaintext loopback or AF_UNIX listener.

## Ownership

- builders copy strings and secrets;
- `server_create` copies the sealed policy and configuration;
- the caller destroys its policy/config after server creation;
- strings returned by a receipt getter are borrowed for that callback only;
- `char **out_error` is owned by the caller and released with
  `maelys_egress_error_free`;
- audit, attestor and TLS providers use explicit retain/release contracts.
- a connector retains only the server control object and authenticated
  principal identity, never the supplied credential;
- a session owns its client fd until `session_take_fd` or `session_release`.

## Threading

`maelys_egress_server_run` blocks its owner thread. Receipt and attestor callbacks
run synchronously on that owner and must not call server control operations.
Another thread may stop the server, atomically replace a sealed policy, read the
policy generation, obtain an immutable metric snapshot or open native connector
sessions. One immutable connector may be shared by concurrent opener threads.

## Minimal sequence

```c
maelys_egress_policy_create(&policy, &error);
maelys_egress_policy_allow_tcp(policy, "github.com", 443, 0, &error);
maelys_egress_policy_require_tls_sni(policy, "github.com", 443, &error);
maelys_egress_policy_seal(policy, &error);

maelys_egress_config_create(&config, &error);
maelys_egress_config_set_listen(config, "127.0.0.1", 0, &error);
maelys_egress_config_set_authentication(
    config, "worker", "a-long-random-secret", &error);
maelys_egress_config_set_native_only(config, 1, &error); /* no proxy listener */

maelys_egress_server_create(policy, config, &server, &error);
maelys_egress_policy_destroy(policy);
maelys_egress_config_destroy(config);

/* Run on the owner thread; another thread eventually calls server_stop. */
maelys_egress_server_run(server, &error);
maelys_egress_server_destroy(server);
```

Error handling is omitted in the fragment only. The complete, compiled version
is `examples/basic_proxy.c`; reload, metrics, audit and attestation examples are
beside it and are built by `make examples-check`.

## Native connector sessions

For the complete API contract—including principal authentication, admission,
descriptor ownership, the three deadlines, concurrency, receipts and the
Executor fd-4 boundary—read the dedicated
[native connector guide](native-connector.md). This section is the compact
embedding recipe.

An embedder that needs only this API should call
`maelys_egress_config_set_native_only(config, 1, &error)`. Egress then creates no
HTTP/SOCKS listener: `server_port()` remains zero, while the same event loop,
policy, guarded relay, accounting and receipt engine services native connector
commands. After starting the owner thread, wait until
`maelys_egress_server_is_running(server)` before opening the first session.

The normal listener APIs are for programs that already speak HTTP proxy or
SOCKS5. An embedding host may instead ask Egress for a plain connected stream:

```text
embedding host
   │ connector_session_open("github.com", 443)
   ▼
private TCP client fd ═══ Egress-owned peer ─► policy / pinned connect / SNI
                                             relay / quota / receipt
```

The returned fd is deliberately **not** the upstream socket. Egress keeps the
other end of a private loopback TCP pair, so every byte still crosses the same
guarded relay as a proxy tunnel. Consequently byte quotas, half-close,
ClientHello validation and terminal receipts remain observations rather than
guesses.

```c
maelys_egress_connector_t *connector = NULL;
maelys_egress_session_t *session = NULL;

/* The same credential configured in the server selects one principal. */
maelys_egress_server_connector_create(
    server, "worker", secret, &connector, &error);

/* Call after the server owner thread has entered server_run(). */
maelys_egress_connector_session_open(
    connector, "github.com", 443, 5000, &session, &error);

int fd = maelys_egress_session_fd(session); /* borrowed, blocking, CLOEXEC, TCP */
/* use fd as an ordinary connected byte stream */
maelys_egress_session_release(session);     /* closes it */
maelys_egress_connector_release(connector);
```

To transfer ownership—for example through an Executor fd-4 broker—use:

```c
int owned_fd = -1;
maelys_egress_session_take_fd(session, &owned_fd, &error);
maelys_egress_session_release(session); /* now releases only the empty handle */
```

`session_open` is a blocking control operation with a finite monotonic timeout.
It returns only after Egress has admitted the exact canonical host/port and
connected to one address pinned in the active policy generation. A policy
denial never returns an fd. Cancellation or server shutdown closes both sides
of a pending private pair. Releasing the connector does not terminate sessions
already handed out; stopping the server does.

If the destination requires TLS SNI, identity validation necessarily occurs
after `session_open`: Egress first returns the relayed stream, then buffers the
application's ClientHello and forwards no byte upstream until the exact SNI is
verified. The open-call timeout is distinct from the configured setup/SNI and
relay-idle deadlines.

The complete compiled example is `examples/native_connector.c`. Egress does not
know about fd 4 or `SCM_RIGHTS`; an optional integration adapter owns that wire
protocol and transfers `session_take_fd()`'s result.

## What `config_set_authentication` does

```c
maelys_egress_config_set_authentication(
    config, "worker", "a-long-random-secret", &error);
```

This installs one authenticated **principal** in the configuration:

```text
principal name: worker
shared secret:  a-long-random-secret
```

It does not authenticate the C application calling the function. It configures
the credential that future proxy clients must present to the Egress listener.
The function copies both strings, replaces any principals previously present
in that configuration, and requires a canonical username plus a visible-ASCII
secret between 16 and 255 bytes.

The same principal can authenticate through either supported proxy protocol:

```sh
# HTTP proxy Basic authentication: username + secret
curl --proxy http://127.0.0.1:8080 \
  --proxy-user 'worker:a-long-random-secret' \
  https://github.com/

# HTTP proxy Bearer authentication: the unique secret is sufficient
curl --proxy http://127.0.0.1:8080 \
  --proxy-header 'Proxy-Authorization: Bearer a-long-random-secret' \
  https://github.com/

# SOCKS5 username/password authentication
curl --socks5-hostname \
  'worker:a-long-random-secret@127.0.0.1:8080' \
  https://github.com/
```

Egress compares credentials without early-exit string comparison. For an HTTP
forward request it removes `Proxy-Authorization` before sending the request to
the origin. Receipts name the matched principal but never contain its secret.

For several independently receipted workloads, add principals instead:

```c
maelys_egress_config_add_principal(
    config, "worker-a", secret_a, "invocation-001", &error);
maelys_egress_config_add_principal(
    config, "worker-b", secret_b, "invocation-002", &error);
maelys_egress_config_set_principal_quota(
    config, "worker-a", 4, UINT64_C(104857600), &error);
```

Secrets must be unique, which makes Bearer authentication map to exactly one
principal. `invocation_id` is a non-secret correlation value copied into
receipts. The quota above permits four active streams for `worker-a` and at
most 100 MiB observed per stream; zero means unlimited.

ABI 2 adds a third, execution-wide ceiling without conflating it with the
per-stream limit:

```c
maelys_egress_config_set_principal_quota_v2(
    config, "worker-a",
    4,                       /* active streams */
    UINT64_C(16777216),      /* each admitted stream */
    UINT64_C(67108864),      /* all streams for this principal */
    &error);
```

The reactor owns the cumulative counter. Every admitted payload read or write
is shortened to the exact remaining allowance before the system call. Proxy
authentication and HTTP/SOCKS framing are not payload and do not consume it.
Receipts expose the stream observation, cumulative value before/after close,
and the exact ceiling that terminated a stream.

Egress copies secrets so it can authenticate after the builder is destroyed. The
embedding host remains responsible for obtaining them from a secret manager,
not logging them, and wiping its own mutable source buffer when possible.

## From configuration to a running server

The handles serve different phases:

```text
policy builder ── seal ──┐
                         ├── server_create ──► independent server copy
config builder ──────────┘                         │
                                                  ▼
                                             server_run
```

`server_create` copies the sealed policy and configuration, including
principals. Destroying the two builders afterward is expected. `server_run`
owns the data-plane thread until another thread calls `server_stop` or an error
occurs. This is why the complete example uses a signal-waiter thread rather
than calling stop from a receipt callback.

## Linking

After `make install`:

```sh
cc -std=c11 example.c $(pkg-config --cflags --libs maelys-egress) -o example
```

The optional Mbed TLS and wolfSSL archives implement the listener TLS seam.
They do not change the policy, relay or receipt API. A runnable TLS and mTLS
walkthrough is in [TLS deployment](tls.md).
