# Operations and durable evidence

Egress 0.7 keeps its data plane and operations plane separate:

```text
HTTP / SOCKS / AF_UNIX proxy             loopback operations listener
             |                                      |
             v                                      v
      bounded proxy parser                 /healthz   /metrics
             |                                      |
      policy generation N                    aggregate atomics
             |
      relayed connection
             |
      receipt callback + durable audit
```

The operations listener has 16 independent, five-second, 4 KiB request slots.
It cannot consume proxy slots, authenticate as a proxy principal, request a
destination or relay bytes. Only exact `GET /healthz HTTP/1.1` and
`GET /metrics HTTP/1.1` requests are served. It binds numeric loopback only.

`/healthz` reports degraded status after a durable-audit failure. `/metrics`
exports aggregate counters only: no host, credential, invocation ID or other
high-cardinality label is exposed. Embedders can obtain the same counters as an
immutable opaque snapshot with `maelys_egress_server_metrics_snapshot`.

## Policy generations

`maelys_egress_server_replace_policy` clones a sealed policy before taking the
server control lock, swaps it atomically, then destroys the old generation.
Admission copies the selected destination, pinned addresses, policy digest and
generation into the connection. Therefore replacement affects only future
admissions and never changes what an existing stream can reach or what its
receipt states.

The standalone SIGHUP path additionally requires every non-policy option to be
unchanged. Failure to parse, resolve, seal or install a replacement leaves the
current generation active.

## Quotas

Quotas belong to authenticated principals, not source IP addresses. A zero
limit is unlimited and preserves pre-0.6 behaviour. The active-stream counter
is incremented only after policy admission and decremented exactly once during
connection teardown. The byte ceiling is per stream and covers both directions
as observed on the client side, including proxy handshake bytes.

## Audit chain

The optional audit handle owns an exclusive append-only file and a copied HMAC
key. Every receipt produces:

- a monotonically increasing sequence;
- the previous record MAC;
- a canonical receipt representation;
- an HMAC-SHA-256 over that canonical representation, which itself ends with
  the previous MAC;
- one unambiguous canonical receipt containing every semantic field.

ABI 2 includes quota scope, stream ceiling/observation and cumulative
before/after values in both the attestor input and the durable canonical
record. A quota denial therefore cannot be detached from the exact counter
that caused it.

The reader accepts one exact JSON field order and rejects duplicates or extra
fields; the MAC-covered canonical value is the sole semantic representation.
The line is fully written and `fdatasync` succeeds before the in-memory chain
advances. Any write or sync failure permanently marks the handle unhealthy and
degrades `/healthz`. On restart, every complete line is reauthenticated before
the file can be extended. Only one process can hold the file lock.

This is symmetric authentication: anyone holding the key can forge a chain.
It is deliberately described as an authenticated receipt journal, not as an
independent public-key attestation. Persist the final chain value in an external
checkpoint if complete-tail deletion must be detectable.

For independently verifiable evidence, an embedder can install a retained
`maelys_egress_attestor` provider. Egress passes a bounded canonical receipt payload
to the provider and records its name, key id and returned signature in both the
borrowed receipt and the HMAC-authenticated audit line. The provider can use
Ed25519, an HSM or a platform keystore; Egress core neither chooses nor links the
algorithm. A signing failure suppresses the durable record and degrades health,
so attestation cannot disappear silently.

## Executor compatibility

All 0.6 operational features and the 0.7 native connector are additive calls
on opaque Egress ABI 2 handles. An Executor
adapter that creates a policy and profile exactly as in 0.5 receives:

- policy generation 1 for its server lifetime;
- unlimited quotas;
- no operations listener;
- no durable audit sink or attestor;
- the unchanged HTTP/SOCKS/AF_UNIX data path.

Executor is not linked to the standalone CLI and does not receive signals from
it. It may opt into replacement, quotas, metrics, audit or attestation
independently later.
