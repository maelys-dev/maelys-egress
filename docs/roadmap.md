# 0.x roadmap

## 0.1 — standard forward proxy

HTTP CONNECT, one-exchange HTTP forward mode, SOCKS5, exact sealed policy,
pinned DNS, loopback authentication, bounded reactor relay, receipts, TLS seam.

## 0.2 — destination identity and relay hardening

Bounded fragmented ClientHello/SNI enforcement, explicit DNS generation
resealing, large-relay half-close/backpressure tests and a mutation gate.

## 0.3 — confinement adapters and resolver isolation

Profile-owned proxy environment, authenticated per-execution credentials and
receipt correlation are delivered. Backend-specific exclusive proxy
reachability remains an Executor adapter capability: unsupported backends must
refuse rather than treating environment variables as confinement. Resolver
isolation moves to the operational gateway because the immutable reseal
contract is already available without adding an unproven async ABI.

## 0.4 — optional TLS listeners

Delivered: Mbed TLS reference module, wolfSSL parity module, remote
authenticated listener, mutual TLS and fail-closed provider configuration. No
MITM.

## 0.5 — filesystem mediation ingress

Delivered: authenticated filesystem `AF_UNIX` listeners, optional same-EUID
peer verification, private-parent validation and replacement-safe teardown.
This is the Egress half of the bubblewrap namespace bridge; the in-namespace
relay/bootstrap remains owned by Executor.

The 0.5.1 standalone increment exposes that listener in the CLI and adds a
strict persistent configuration plus systemd, launchd and container examples.

## 0.6 — operational gateway

Delivered: atomic policy generations, per-principal connection and byte
ceilings, aggregate metric snapshots, a loopback health/Prometheus endpoint,
and a restart-verifying durable HMAC receipt chain. The HMAC chain is
authenticated shared-key evidence, not public-key attestation; asymmetric
attestation is supplied through the delivered generic provider seam so the
minimal binary does not choose Ed25519/HSM/platform-key dependencies.

The 0.6.1 product increment adds compiled native examples, standalone Python
and Node.js process helpers, and a single documentation path covering CLI,
embedding and Sandbox–Executor–Egress integration. It does not add
a mutable HTTP control endpoint.

## 0.7 — native relayed connector

Delivered: an authenticated opaque connector binds one configured
principal and opens concurrent native sessions through the same owner-thread
admission, pinned connection, SNI guard, quota, half-close, metrics and receipt
state machine as HTTP and SOCKS. Each successful open returns the blocking
CLOEXEC client end of a private loopback TCP pair; Egress retains the accepted
peer and never exposes the upstream socket.

This is the Egress half of full-semantics fd-4 convergence. Egress remains unaware
of Executor and `SCM_RIGHTS`; the optional Executor-Egress adapter must translate
the published fd-4 request into a connector session and transfer its fd. The
existing standard-client Bubblewrap and Seatbelt proxy routes are unaffected.

## 0.8 — endpoint-bound execution principal

Delivered: a private AF_UNIX listener can be immutably bound to exactly one
principal without giving the proxy client a bearer credential. The listener's
private pathname and socket identity define the execution endpoint;
`SO_PEERCRED`/`getpeereid` same-EUID verification remains an additional check,
not the per-execution identity. HTTP and SOCKS clients use conventional proxy
URLs without userinfo, while Egress applies the bound principal's policy, quota,
invocation correlation and receipts. Credential-bearing requests are refused
on this mode so the two authentication models cannot be confused.

## 0.9 — Egress namespace

Delivered: the product, binaries, libraries, headers, symbols, TLS modules,
SDK packages, metrics and services renamed from the former product name to
Maelys Egress under one `egress` namespace with a new product ABI, without compatibility
aliases; wire protocols, configuration keys and security guarantees unchanged.

## 0.10 — execution-cumulative quotas

Delivered: Egress ABI 2 adds a per-principal cumulative byte quota beside the
active-stream and per-stream ceilings, accounted exactly by the owner reactor
and capped before every payload system call, with receipts, CLI JSON, audit
records and attestation carrying the limiting scope. The 0.10.1 increment
advances the pinned Maelys System to 0.5 while keeping ABI 2.

## 0.11 — executable CLI contract

Delivered: explicit `help`, `version`, `describe`, `config describe`,
`config validate`, `completion` and `serve` commands over one strict
configuration file, a compiled catalog, machine-readable discovery, the
versioned `maelys-egress-lifecycle/1` stdout stream, generated references
verified in CI, and process SDKs that drain that stream.

## 0.12 — shared CLI framework and MPL-2.0

Delivered: the command line is built on the pinned `libmaelys_cli` framework
and aligned with the `agent-cli/v2` vocabulary of Maelys Git and Hermes;
`config validate` reports violations with exit 2; the server and CLI sources
are split by feature; one canonical receipt encoder serves attestation and
the audit journal; the configuration file loads into typed settings; the
lifecycle stream has one writer; emitted envelopes, data and lifecycle events
are validated against the committed schemas in CI; the repository, its SDKs
and the pinned Maelys System are relicensed under MPL-2.0.

## 0.13 — packaged distribution

Delivered: releases go through the shared `maelys-release` socle (signed tag,
three targets, provenance, GitHub release, Homebrew formula with bottles);
Maelys System is pinned by tag and commit and may be linked already
installed (`MAELYS_SYSTEM_PREFIX`), so the Homebrew formula depends on the
tap's `libmaelys-sys` instead of vendoring it; every installation registers
the daemon with the `maelys` dispatcher through `share/maelys/commands/egress.json`.

## 0.14 — sockets through Maelys System

Delivered: every socket of the server is a Maelys System handle, from the
listeners and accepted clients to the upstream connects, the relay's
receive, send and half-close, the operations listener and both ends of the
private connector pair (`connect_start`, `connect_complete`, `detach`); the
boundary audit refuses native socket calls in `src/`. A peer half-close is
handled as readability, so no queued byte is lost and no level-triggered
`HUP` spins. Releases go through maelys-release 0.5.0 with the managed
dependency checkout and the tag preflight.

## 0.15 — file primitives from the dependencies

Delivered: the audit journal is held through Maelys System's
identity-checked lock (`file_lock_acquire`, verified before and after the
lock) and the Unix socket path is retired with `unlink_same`; the command
reads its configuration and secrets through the maelys-cli trusted reader,
which judges the descriptor it reads. No `open`, `fstat`, `lstat`, `flock`
or `unlink` of Egress's own remains, and the boundary audit refuses them.

TLS inspection, transparent interception and content inspection have no
release number until their PKI and threat-model design is accepted
adversarially. If accepted, inspection remains an explicit isolated capability,
not a default property of the Egress core.
