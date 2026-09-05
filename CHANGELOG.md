# Changelog

## Unreleased

- Adopt maelys-release 0.10.0: the managed texts carry no socle version,
  the provenance attestation follows the repository's visibility, and CI
  calls the socle's `check-product.yml` (checkouts, packages, `make check`
  on the three release targets, drift check) as one job next to the
  product's own; the hand-written drift step is gone.
- Adopt maelys-release 0.13.0 (socle on the Python framework of maelys-cli
  0.5.14, agent-cli-spec 2.2.0): only the two `uses:` lines change.
- Adopt maelys-release 0.14.0: the declarations move from `adapter/` to
  `dependencies/` (`maelys-system.pin`, `maelys-cli.pin`, `packages`); the
  Makefile, the packaging and installed-System scripts, the formula renderer
  and the documentation read them there.

## 0.15.0 — 2026-09-05

- Pin maelys-cli `v0.5.11` and read the configuration file, the token file
  and the audit key through the framework's trusted reader
  (`maelys_cli_open_trusted`, `maelys_cli_read_trusted_file`): the file
  judged is the descriptor read, the open never blocks on a planted FIFO,
  and secrets require the caller as owner, a single hard link and no group
  or world bit, bounded by the bytes actually read. The command carries no
  `open`, `fstat` or `O_NOFOLLOW` of its own; the boundary audit refuses
  them in `cli/`. Error codes of `config validate` are unchanged.
- Pin Maelys System `v0.8.0` (`93103a1d0297ea3f334cbc84079c93be6e9b0efd`)
  and consume its file primitives. The audit journal is held through
  `maelys_sys_file_lock_acquire`: owner, single link, owner-only mode and
  regular file are verified before the exclusive lock and again after it,
  with the path re-resolved to the locked inode, which the previous
  `open`, `fstat`, `flock` sequence could not do. The Unix socket path is
  retired with `maelys_sys_file_unlink_same` against the identity captured
  at bind, instead of an `lstat` followed by `unlink`; the check and the
  removal remain two calls, as System's contract states. The boundary audit
  refuses `lstat`, `flock` and `unlink` in `src/`.
## 0.14.0 — 2026-09-04

- Pin Maelys System `v0.5.6` (`31e52fa210d86851b141ecd75e8e2231c0d15ae2`).
  The private connector pair is built entirely through System: both ends are
  `socket_create`, the embedder's end goes through `connect_start`, a
  private reactor waiting for writability, `connect_complete`, then
  `maelys_sys_socket_detach` and `maelys_sys_fd_set_blocking` before it is
  handed over as the blocking TCP descriptor the contract promises. No
  native socket call remains in the server; the Unix peer identity read
  stays on the native descriptor by design, and the boundary audit now
  refuses every native socket call in `src/`.
- Adopt maelys-release 0.5.0: the managed `scripts/checkout-dependency.sh`
  replaces the product's `checkout-system.sh` and `checkout-cli.sh`,
  `adapter/PACKAGES` declares the runner packages (none beyond the socle's
  own), CI checks socle drift with `maelys-release check`, and the release
  workflow is regenerated.
- Pin Maelys System `v0.5.5` (`97231ceb6b8ee29625838fe15787e6c336ba6105`) and
  bind listeners with `maelys_sys_socket_bind_with` (`reuse_address`), the
  last network `setsockopt` of the server; the boundary audit now refuses it
  outside the connector's bare embedder end.
- A peer half-close (`HUP`) is handled as readability, not as an end of
  stream: queued bytes are read until System reports the socket closed, and
  when the read buffer is full the watch is dropped until upstream progress
  frees room, so a level-triggered `HUP` neither loses the tail nor spins.
  Every System backend now reports half-closes this way (poll gained
  `POLLRDHUP` in 0.5.5). The operations listener answers a client that
  half-closes after its request instead of dropping it.
- Sockets go through Maelys System: listeners, accepted clients, upstream
  connects, the relay's receive, send and half-close, the operations listener
  and the relayed end of the private connector pair are `maelys_sys_socket_t`
  handles, created non-blocking, close-on-exec and SIGPIPE-safe by System and
  completed with `connect_start`/`connect_complete` instead of `SO_ERROR`.
  The reactor and TLS providers keep borrowing the native descriptors. The
  only bare socket left is the blocking TCP end handed to the embedder by the
  connector, which a System handle cannot give up; `SO_REUSEADDR` on
  listeners and the Unix peer identity checks use the native descriptor
  because System exposes no socket options. `scripts/audit-boundaries.sh`
  refuses raw socket calls elsewhere and `system-integration-check` requires
  the socket symbols.
- Behaviour note: a peer reset while receiving now ends the stream like an
  orderly EOF (System reports both as closed) and propagates as a half-close
  instead of failing the connection with an I/O result.
- Regenerate the release workflow with maelys-release 0.2.8 (the tap publish
  job styles the merged formula inside its staging tap; the 0.13.1 tap
  publication was replayed with it).

## 0.13.1 — 2026-09-03

- Regenerate the release workflow with maelys-release 0.2.7 (the tap publish
  job no longer trips on the previous formula of the shared tap).
- Pin Maelys System `v0.5.4` (`07e8ad33950f07049096ca33a2ebf90c5a2039ca`): the
  public System repository restarted its history under MPL-2.0 and the
  `v0.5.3` release Egress 0.13.0 pinned now lives in a private archive. The
  Homebrew formula follows the `libmaelys-sys` 0.5.4 bottles.
- Regenerate the release workflow with maelys-release 0.2.6: the shared tap
  is tapped before bottles are built (the `maelys-egress` bottles of 0.13.0
  failed to find `libmaelys-sys`), and `workflow_dispatch` with a `tag`
  input replays the Homebrew publication of an existing tag.

## 0.13.0 — 2026-09-03

- Pin Maelys System by tag and commit (`v0.5.3`,
  `8fe2924da268f742b8071c6557e4bb0d6d6ad116`, the first System release
  published through the shared socle); the pinned version replaces the
  `0.5.0` the Makefile and `scripts/package-release.sh` hard-coded.
- `MAELYS_SYSTEM_PREFIX` builds and installs against an already installed
  Maelys System (ABI 1, at least the pinned version) instead of the pinned
  checkout, and then installs neither `libmaelys_sys` nor its headers. The
  Homebrew formula uses it: it now depends on the tap's `libmaelys-sys`
  instead of vendoring System, and no longer conflicts with `maelys-warden`.
- `make install` writes `share/maelys/commands/egress.json`, the
  `maelys.cli-extension/v1` manifest that registers the daemon as
  `maelys egress` for the maelys-cli dispatcher.
- `make install-check` aborts on the first missing file instead of only
  reporting the last test.
- Regenerate the release workflow with maelys-release 0.2.5, which declares
  the permission ceiling GitHub requires of a workflow calling reusable
  workflows, grants the tap job what its bottle attestation needs, trusts
  the staging tap before merging the bottle digests and sums only the
  archives the product built.
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
