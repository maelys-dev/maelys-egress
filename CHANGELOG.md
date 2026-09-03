# Changelog

## Unreleased

- Regenerate the release workflow with maelys-release 0.2.2, which declares
  the permission ceiling GitHub requires of a workflow calling reusable
  workflows.
- Adopt maelys-release 0.2.0 through its `adopt.sh`: the release workflow
  is generated (bottles for macOS 15 and 26, tap credentials optional), the
  maelys-release agent block, Claude skill and `RELEASING.md` are installed,
  and CI verifies with `adopt.sh --check` that the workflow has not drifted
  from the pinned socle.
- Release through the shared `maelys-dev/maelys-release` workflows (signed
  tag, three-target packaging, provenance, GitHub release) and publish a
  Homebrew formula to `maelys-dev/homebrew-tap`: `packaging/homebrew/
  maelys-egress.rb.in` is rendered from the released tag with the tag's own
  System and CLI pins by `scripts/render-homebrew-formula.sh` (`make
  package-homebrew`). The formula builds from source, installs the daemon,
  libraries and headers, and conflicts with `maelys-warden` until the tap
  has a shared `maelys-system` formula.
- Speed up the gates without touching the code: the boundary audit uses
  POSIX `grep -E`, so ripgrep is no longer installed on any runner or image;
  the five mutants of `scripts/mutation-check.sh` build and test in parallel
  over one shared dependency build.
- Advance the pinned Maelys CLI framework to `v0.5.1` and adopt its new
  surface: shell completion is the framework built-in generated from the
  catalog (the product `completion` command and its schema are removed;
  `maelys-egress completion bash|zsh|fish` keeps working), the reference
  generator is release-neutral by default (the `0.0.0` contract build is
  gone), `MAELYS_CLI_FORMAT=json` turns a `serve` startup failure into an
  `agent-cli/v2` envelope on stderr and the Python and Node.js SDKs set it,
  the writer refuses invalid UTF-8 and `NULL`, and the framework agent
  instructions are installed (`maelys agents install`).
- Advance the Maelys System pin from the relicense branch commit `7a5b232`
  to its `main` merge commit `cbe08b3`; identical sources, ABI 1 and 0.5.0.

## 0.12.0 — 2026-09-02

- Build the command line on the shared `libmaelys_cli` framework, pinned to
  `maelys-dev/maelys-cli` tag `v0.1.0` through `adapter/MAELYS_CLI_PIN` and
  `scripts/checkout-cli.sh`. This is a breaking 0.x change of the CLI
  contract, not of the C ABI: `describe` now returns the framework shape
  (`id`, `pattern`, `input`, `outputSchema`, `exitCodes`, global options),
  error codes are the eleven stable framework codes in upper case, exit `2`
  is reserved for a completed validation report with violations, and
  `version` prints `maelys-egress X.Y.Z`. `config validate` reports
  configuration problems as `data.valid: false` with `data.diagnostics` and
  exit `2`; `serve` is a `protocol-stream` command owning stdout for the
  unchanged `maelys-egress-lifecycle/1` stream. `config describe` gains
  `tlsListener`. The generated references move to the framework generator
  (`docs/generated/cli-contract.json` added) and `make contract-check`.
  Configuration keys, the lifecycle events, the SDKs' process contract and
  the library ABI 2 are unchanged.
- Relicense the repository, including the Python and Node.js SDK packages,
  from MIT to MPL-2.0 (`LICENSE`, `LICENSING.md`, package metadata). Binary
  packages are labelled `MPL-2.0`. Advance the Maelys System pin to
  `7a5b232`, the MPL-2.0 relicense of the unchanged 0.5.0 ABI 1 contract.
- Make `src/receipt.c` the single canonical receipt encoder: the attestor
  input and the audit journal core are now the same function, pinned by a
  unit test, with unchanged bytes so existing signatures and journals stay
  verifiable.
- Load the configuration file straight into a typed `egress_cli_settings_t`
  (`cli/config_file.c`), with values and cross-key constraints checked at
  load time and reported with their line; `serve`, `config validate` and
  SIGHUP reload consume the same structure, and reload compares control-plane
  fields instead of a serialized argument signature. The internal option
  vector and its second parser are gone.
- Confine the lifecycle stream to `cli/output.c`, now built with the
  framework JSON writer under one mutex; `scripts/audit-boundaries.sh`
  refuses any other stdout writer under `cli/`.
- Validate what the binary emits against the committed schemas in
  `make check` (`tools/check_schemas.py`): envelopes, command `data` and a
  complete `serve` lifecycle run. Bring `docs/roadmap.md` up to 0.12.
- Split `src/server.c` into feature files (lifecycle, listeners, connection
  admission, relay, quotas, receipts, native connector commands, admin
  listener) behind the private `src/server_internal.h` contract, and split
  `cli/maelys-egress.c` into dispatcher, discovery, configuration, secrets,
  serve, reload and output modules behind `cli/cli.h`. The TLS module
  selection now lives in `cli/tls_listener.c` alone and is queried at run
  time, so every other CLI file is compiled once. No public ABI, command,
  configuration key, wire format, lifecycle event or exit code changes; the
  split exists to keep each audit and review scoped to one concern.

## 0.11.0 — 2026-09-01

- Replace the ambiguous option-only daemon invocation with explicit `help`,
  `version`, `describe`, `config describe`, `config validate`, `completion` and
  `serve` commands. Operational settings now come exclusively from one strict
  configuration file declaring `schema_version = 1`; no compatibility aliases
  preserve the former 0.x spellings.
- Add a compiled command/configuration catalog, executable `agent-cli/v2`
  discovery, causal JSON diagnostics, generated Markdown references and
  bash/zsh/fish completions. CI verifies that every published command has a
  dispatcher and that checked-in references match the binary.
- Replace the text `READY` line and stderr receipts with the versioned
  `maelys-egress-lifecycle/1` stdout JSONL stream. Readiness, policy reloads,
  receipts, bounded shutdown and fatal events now share one parseable contract.
- Update the dependency-free Python and Node process SDKs to launch `serve`,
  validate and continuously drain lifecycle events, expose receipt/reload
  consumption, and prevent stdout backpressure after readiness.
- Add the repository `egress-cli-contract` skill and agent rules so future
  command/configuration changes update the catalog, protocols, SDKs, tests and
  generated documentation together. Public Egress C ABI 2 is unchanged.

## 0.10.1 — 2026-09-01

- Advance the exact Maelys System dependency to 0.5.0 while preserving Egress
  ABI 2 and all proxy, policy, quota and receipt behavior. System 0.5 remains
  ABI 1 and adds only opaque socket mechanics, so this compatibility release
  lets Egress and Warden link one verified System closure without duplicate or
  conflicting pins.
- Keep the Egress reactor and raw-descriptor ownership unchanged. Migrating
  those internals to the new optional System socket handles is deliberately
  deferred to a separately testable release rather than hidden in a dependency
  correction.

## 0.10.0 — 2026-08-30

- Introduce Egress ABI 2 with an execution-cumulative byte quota beside the
  existing active-stream and per-stream ceilings. The single owner reactor
  accounts concurrent and sequential streams exactly and caps every payload
  I/O to the remaining sealed allowance before issuing the system call.
- Exclude proxy authentication and HTTP/SOCKS framing from relay-payload
  accounting. Extend callback, CLI JSON, asymmetric attestation and durable
  HMAC receipts with the limiting scope and per-stream/cumulative evidence.
- Add the `quota_total_bytes` standalone setting and adversarial native
  connector coverage proving that per-stream and cumulative limits remain
  independent and never overshoot.

## 0.9.0 — 2026-08-30

- Rename the product and every public surface from Maelys Netd to Maelys
  Egress: repository metadata, binaries, libraries, C headers and symbols,
  TLS modules, SDK packages, metrics, services, configuration examples,
  release assets and documentation now use one `egress` namespace.
- Establish `MAELYS_EGRESS_ABI_VERSION` 1 as the new product ABI. The rename is
  intentionally source- and link-incompatible with the previous namespace;
  no compatibility aliases or duplicate binaries are shipped.
- Keep the wire protocols, configuration keys, policy semantics, security
  guarantees and runtime behaviour unchanged by the naming transition.

## 0.8.0 — 2026-08-28

- Add an endpoint-bound principal mode for execution-private AF_UNIX
  listeners. The listener identity selects exactly one principal, while
  SAME_EUID remains an additional transport-peer check.
- Add credential-free HTTP/SOCKS profiles whose standard proxy URLs contain
  no userinfo. Explicit `Proxy-Authorization` is rejected on an endpoint-bound
  listener instead of being ignored.
- Preserve per-principal connection and byte quotas, receipts, invocation
  correlation and the complete policy engine without transporting a bearer
  secret through a sandbox or VM guest environment.
- Keep generic TCP, remote TLS, standalone AF_UNIX and native connector modes
  credential-authenticated. Endpoint binding is opt-in, AF_UNIX-only and
  requires a private owner-only listener parent plus same-EUID verification.
- Keep Netd ABI 1 additive and cover immutable configuration, HTTP, SOCKS,
  credential rejection, credential-free environment and quota assignment.

## 0.7.0 — 2026-08-26

- Add an authenticated opaque connector/session API for native embedders.
  Successful opens return a blocking CLOEXEC private TCP client stream while
  Netd retains the peer in its normal guarded relay; the upstream socket is
  never transferred.
- Route native requests through the same exact policy generation, pinned
  addresses, optional SNI guard, per-principal quotas, half-close, metrics,
  durable audit and receipt machinery as HTTP CONNECT and SOCKS5.
- Add bounded cross-thread session admission with concurrent open support,
  cancellation on deadline/server stop and retained server-control lifetime.
- Exercise authentication failure, canonical-host rejection, policy denial,
  stream kind, SNI non-bypass, byte observation, idle-server timeout and eight
  concurrent connector sessions.
- Keep Netd ABI 1 additive and keep fd 4, `SCM_RIGHTS`, Executor and Sandbox out
  of the core library; their adapter consumes this generic connector seam.
- Add native-only embedding mode, an explicit server-readiness predicate and a
  dedicated guide covering authentication, admission, SNI, ownership,
  deadlines, concurrency, receipts and the Executor boundary with diagrams.

## 0.6.1 — 2026-08-24

- Add a product-oriented documentation path from first CLI request through
  standalone operations, native embedding and Sandbox–Executor integration.
- Add five compiled C examples covering a complete proxy, policy replacement,
  metrics snapshots, durable audit and the generic attestor provider seam.
- Add dependency-free Python and Node.js process SDKs that create private
  credentials/configuration, parse the bounded `READY` contract, expose proxy
  URLs, query health/metrics and perform policy-only SIGHUP reload. Versioned
  source packages ship as release assets without claiming PyPI/npm publication.
- Correct the architecture threading contract and distinguish callback-only,
  HMAC-authenticated and asymmetrically attested receipts in the security model.
- Keep Netd ABI 1 and the proxy/admin wire contracts unchanged; the SDKs use
  existing standalone surfaces rather than adding a mutable HTTP control API.

## 0.6.0 — 2026-08-24

- Add atomic sealed-policy replacement for new admissions; active streams keep
  an immutable destination, policy digest and generation snapshot.
- Add per-principal active-stream and per-stream byte quotas, disabled by
  default, with explicit quota-denial counters.
- Add a separate loopback-only operations listener serving bounded `/healthz`
  JSON and aggregate Prometheus `/metrics` without entering the proxy parser.
- Add synchronous durable JSONL audit with exclusive owner-only files,
  HMAC-SHA-256 chain authentication, `fdatasync` per receipt, restart-time
  verification and fail-closed corruption detection.
- Add a retained generic receipt-attestor seam for Ed25519, HSM or platform
  signers; attestor identity, key id and signature are covered by the durable
  HMAC record without imposing an asymmetric library on the minimal binary.
- Add SIGHUP policy reload to the standalone daemon. Only the repeatable
  `allow*` entries may change; listener, credentials, quotas, TLS and audit
  settings must remain byte-for-byte equivalent.
- Keep Netd ABI 1 and every new feature opt-in; existing Executor profiles and
  adapters retain their previous immutable-policy, unlimited-quota behaviour.

## 0.5.1 — 2026-08-24

- Expose the filesystem `AF_UNIX` listener through the standalone daemon with
  `--listen-unix` and the explicit `authenticated`/`same-euid` peer policy.
- Add a strict, ownership-checked `key = value` configuration file and
  `--check-config`; command-line and file policy sources cannot be mixed.
- Add adversarial CLI coverage for unsafe configuration permissions, duplicate
  scalar keys, source mixing, authenticated Unix startup and identity-safe
  socket teardown.
- Ship configuration, hardened systemd, launchd and container-sidecar examples
  with the release packages.

## 0.5.0 — 2026-08-24

- Add a public filesystem `AF_UNIX` listener for namespace-bridge and sidecar
  deployments while retaining mandatory HTTP/SOCKS proxy authentication.
- Require an absolute canonical socket path beneath a caller-owned mode-0700
  directory, reject symlinks and pre-existing nodes, and revalidate the parent
  immediately before binding.
- Add an explicit peer policy: application authentication alone or application
  authentication plus same-effective-UID verification through `SO_PEERCRED` or
  `getpeereid`.
- Create socket nodes mode 0600 and remove them only when their device/inode
  identity still matches, preserving a replacement created by another owner.
- Exercise HTTP CONNECT and SOCKS5, failed authentication, receipts, unsafe
  paths, changed permissions and replacement-safe teardown over the Unix
  transport.
- Clarify the single-enforcement-engine integration with Executor and the ECH
  security posture, including the distinct RFC 9849 split mode available only
  to controlled or cooperating origins.

## 0.4.1 — 2026-08-23

- Serialize the cross-thread `server_stop` wakeup with owner-thread reactor
  destruction, closing an intermittent use-after-destroy race found by the
  tag's independent TSan rerun.

## 0.4.0 — 2026-08-23

- Connect the TLS provider ABI to the listener state machine with bounded,
  readiness-driven handshake, record I/O and close-notify handling.
- Permit remote listeners only when an explicit TLS provider and proxy
  authentication are both configured; plaintext listeners remain loopback-only.
- Add optional Mbed TLS and wolfSSL reference modules with server/client roles,
  hostname verification, trust stores and mutual TLS.
- Add provider-specific standalone binaries without adding either TLS stack to
  the minimal `maelys-netd` dependency closure.
- Exercise the generic listener path end-to-end and both real providers with a
  common mutual-TLS handshake and encrypted record test.

## 0.3.0 — 2026-08-23

- Add up to 64 independently authenticated principals with unique secrets and
  canonical invocation IDs, preserving constant-time credential checks.
- Correlate HTTP and SOCKS authentication with invocation IDs carried into
  credential-free connection receipts.
- Add `maelys_netd_profile`, an optional Executor-facing seam that owns one
  credential, produces closed standard proxy environment entries and applies
  the matching server principal without exposing the secret through getters.
- Percent-encode proxy credentials, explicitly clear `NO_PROXY`/`no_proxy`,
  and keep Netd core independent from Executor and MCP Runtime.

## 0.2.0 — 2026-08-23

- Add an opt-in, bounded and fragmentation-safe TLS ClientHello guard that
  requires one exact canonical SNI before forwarding tunnel bytes.
- Include the SNI identity requirement in the immutable policy digest and
  record successful verification in connection receipts.
- Add fresh-policy resealing so operators can replace pinned DNS generations
  without mutating an active policy.
- Exercise 512 KiB relays under backpressure with bidirectional half-close,
  fragmented ClientHello integration and SHA-256 known-answer vectors.
- Add a permanent five-mutant security gate covering SNI, authority equality,
  credentials, destination ports and half-close propagation.

## 0.1.1 — 2026-08-23

- Advance the pinned Maelys System dependency to v0.4.0, inheriting the
  deadline-enforcement and per-call SIGPIPE hardening without an ABI change.
- Verify Netd as a real link-time consumer of the standalone System ABI 1.

## 0.1.0 — 2026-08-23

- Establish public C ABI 1 with opaque policy, config, server, receipt and TLS
  provider handles.
- Add authenticated loopback HTTP CONNECT, one-exchange HTTP/1.1 forward proxy
  and SOCKS5 with exact destination/port enforcement.
- Resolve and pin allowed destinations at policy sealing, denying non-global
  addresses unless each destination opts in explicitly.
- Build bounded, level-triggered relay state over pinned maelys-system v0.3.0
  poll/epoll/kqueue semantics with backpressure, deadlines and half-closes.
- Add canonical policy SHA-256 identity and per-connection receipt callbacks.
- Publish a backend-neutral nonblocking TLS seam while keeping the 0.1 core
  independent from Mbed TLS, wolfSSL and MITM concerns.
- Add independent ASan/UBSan-backed and libFuzzer targets for both HTTP and
  SOCKS protocol boundaries.
