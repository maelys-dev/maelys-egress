# Changelog

## 0.19.9 — 2026-09-11

- Adopt maelys-release 0.36.0, ten versions on from 0.26.0. Only the workflow
  pins and the managed agent texts change here; the socle asks nothing new of
  this product. `check`, `preflight` and `rehearse` of a current socle now
  re-execute themselves from the socle a product pins, so the answer no longer
  depends on which checkout a developer happens to have — the drift that cost
  three failed gate runs here today.
- `.github/workflows/ci.yml` runs once per pull request instead of twice. Its
  `push:` trigger named no branch while `pull_request:` was declared too, so
  every push of a pull request started both. `branches: [main]` leaves one run
  per pull request and one per push to main. Advised by the socle's adoption,
  and worth having the day a billing lock made every CI minute count.
- `.claude/skills/maelys-release/SKILL.md` carries its CC-BY-4.0 notice, which
  arrived with this adoption. Both installed skills now have theirs.

## 0.19.8 — 2026-09-11

- Adopt agent-cli-spec 2.4.0 and maelys-cli 0.5.24. The specification adds
  `--field NAME` to the trunk of global options: it renders one top-level
  member of `data` so a reader gets a member without a query tool. The trunk
  belongs to the framework, so the move needed maelys-cli first — `--field`
  arrives there in 0.5.23 — and pinning the specification alone would only
  have failed the kit. No product code changes: the conformance kit goes from
  244 to 259 checks, all passing, and `docs/cli-contract.json` gains the
  option. The five framework versions crossed also bring maelys-json 0.1.6 and
  a manifest error that names its failing value by RFC 6901 pointer.
- The four texts `maelys agents install` writes carry their CC-BY-4.0 notice,
  which arrived with this adoption rather than through a pull request, as
  maelys-platform's licensing policy prescribes for an installed text.

## 0.19.7 — 2026-09-11

- `scripts/package-release.sh` reads each artifact back and checks that the
  manifest it contains describes the binary beside it: same digest, same
  declared path, same version. `make check` proves the rules produce a matching
  pair, but it proves it of a staging the test builds and throws away; what
  ships comes from a second `make install`, and the packages from a third. The
  archive is unpacked and read rather than the tree it came from, because a tar
  that dropped or truncated one of the two files would leave the tree correct
  and the artifact refused by the dispatcher. The Debian package is read back
  the same way; the RPM is built from the staging that is checked before it.

## 0.19.6 — 2026-09-11

- `.claude/skills/egress-cli-contract/SKILL.md` is CC-BY-4.0, attributed to
  David Bromberg, and carries its own notice: SPDX identifier, copyright,
  source and license link. It was MPL-2.0 through this repository's blanket
  clause. maelys-platform's licensing policy licenses every agent text of a
  Maelys repository under CC-BY-4.0, whether a socle installed it or the
  product wrote it, on the ground that what decides is the nature of the text
  rather than who shipped it, and that an attribution which does not travel
  with the copy is not one. 0.19.4 had drawn the line the other way, naming
  the two installed skills and leaving ours under the MPL; `LICENSING.md`
  states the new rule for both sides. The two installed skills receive their
  notice at the next adoption of the distributions that write them, never
  through a pull request of this repository.

## 0.19.5 — 2026-09-10

- `maelys egress` is bound to the binary it registers. The dispatcher manifest
  now carries the `sha256` of the executable, and the dispatcher of the pinned
  maelys-cli refuses to run an executable whose digest does not match. Without
  it the manifest was an alias: a different program left at the declared path
  ran under our name, which a live check confirmed before the change by
  substituting one and watching `maelys egress --version` answer for it. The
  template moves from `packaging/maelys/egress.json.in` to
  `cli/command.json.in`, with the terminal it describes, as maelys-platform's
  layout policy asks.
- The manifest and the pkg-config file are rendered by
  `scripts/render-install-metadata.py` on every `make all` and `make install`
  rather than from timestamps: both name the installation, and `PREFIX` or the
  linker flags change them without any source moving. Unchanged bytes keep
  their mtime. `install-metadata-check` installs under two prefixes without
  cleaning and reads back what the dispatcher would read.
- Every object follows the version it embeds. `CPPFLAGS` writes it into each
  object as `MAELYS_EGRESS_BUILD_VERSION`, but no object depended on it, so a
  bump left the binary reporting the previous version until something else
  forced a rebuild. The objects now follow a stamp file holding the value in
  use, rewritten only when that value differs: a prerequisite on `VERSION`
  alone would have missed `make VERSION=x`, which changes neither that file nor
  its timestamp. A release built from a clean tree was never affected; a local
  build always was, and the manifest would have declared a version its binary
  contradicted.
- `all` is declared before `-include $(DEPENDENCIES)`. A rule read from an
  included makefile becomes the default goal, so once the dependency files
  existed, plain `make` built a single object and stopped: touching a source
  changed nothing, and the tree drifted from its sources without saying so.
  Clean builds, which is what CI and every release do, were never affected,
  which is why nothing caught it. Found while trying to observe the version
  staleness above.

- `sdk-check` compares the two SDK `README.md` to `VERSION`, and says which
  file is wrong when a comparison fails. It already held the three files that
  declare the version, but not the READMEs, whose install commands name the
  released archive by version and ship inside the SDK archives themselves; a
  release could have told a reader to install the previous version. The three
  existing comparisons were silent `grep`s, so a stale file failed the build
  with `Error 1` and nothing else.

## 0.19.4 — 2026-09-10

- The copyright holder is David Bromberg, in `LICENSING.md` and in the
  `Maintainer` field of the Debian package. 0.19.3 and every release before it
  installed `share/doc/maelys-egress/LICENSING.md` naming "Maelys Developers",
  an entity that does not exist, and the `.deb` control file carried the same
  name. The rest of the fleet still does; only this product is corrected.
- `LICENSING.md` says which license covers each agent text. The CC-BY-4.0
  section lists the two skills a Maelys distribution installs,
  `maelys-cli-command` and `maelys-release`, rather than the `.claude/`
  directory, which would have placed a text written here under a grant made
  by someone else; the MPL-2.0 section now names the skills written in this
  repository, so both sides read explicitly. The line is distribution, not
  prose against code: maelys-cli draws it the same way, keeping its own
  `maelys-cli-framework` skill outside the `share/agents/` it distributes.
- `README.md` names nothing in place of the documentation it no longer
  carries. `maelys-release migrate` wrote a pointer to `maelys-dev/maelys-docs`
  in 0.18.4, which is private and leads nowhere for a reader outside the
  organisation; maelys-platform's own documentation policy says a README
  points at the product's own site and never names `maelys-docs`. No public
  site is declared for this product, so the section goes rather than pointing
  at what cannot be opened. Reported by the maelys-oci session, which made
  the same correction.

## 0.19.3 — 2026-09-10

- Adopt maelys-release 0.26.0 and hand both fuzz targets to its job:
  `make fuzz-smoke fuzz`, the corpus replay then the bounded run. This
  repository stops fuzzing in a job of its own. It became possible in two
  steps: 0.25.1 gave that job the compiler runtime a libFuzzer harness needs,
  and 0.26.0 rewrote the fuzzing conventions against a survey of the fleet,
  which found that a bounded run belongs in CI and that this product's
  `-runs=10000` is one.

## 0.19.2 — 2026-09-10

- Adopt maelys-release 0.25.1. Its fuzz job installs the compiler runtime the
  sanitizers job beside it already had, so a smoke target written as a
  libFuzzer binary can link there; this product spells its own as a
  standalone driver and was never affected, which is why the report that led
  to the fix described the wrong consequence. The campaign stays out of CI,
  on the conventions' own terms rather than for want of a library.
- The versions in between add declarations this product does not make: the
  release targets and the manifest kinds become `packaging/release` inputs
  with the socle's own values as defaults, and a registry channel is opt-in
  through a file this repository does not carry. Only the pinned commits of
  the three called workflows change here.

## 0.19.1 — 2026-09-10

- Adopt maelys-release 0.23.0, whose `migrate` now rewrites every Markdown
  file it reports rather than the README alone, and names a rule matching
  `docs/` as a whole before a migration changes what it matches. Both were
  reported from this repository.
- The socle's fuzz job replays the committed corpus, declared as
  `make fuzz-smoke`. CI ran no smoke replay until now: the corpus was
  exercised only by the libFuzzer campaign, which stays in this
  repository's own job because the socle's installs no compiler runtime for
  it. The two are complementary, not a duplicate.

## 0.19.0 — 2026-09-10

- Adopt maelys-release 0.22.1. The generated CLI reference belongs to the
  socle now: it runs maelys-cli's generator at this product's pinned commit
  and reads the new `docs/cli.reference` for the build holding the programs,
  so `docs/generated/cli-reference.md` becomes `docs/cli.md` and the contract
  `docs/cli-contract.json`, the path every product uses. Both are installed
  beside the other documents rather than under `generated/`, which keeps the
  configuration reference this product still generates itself with
  `make config-reference`. The local `cli-reference` target is gone and
  `contract-check` compares what remains ours.
- A broken third-party apt source of the runner image no longer fails the
  shared CI job either: 0.18.4 fixed the five workflows this repository owns,
  and the socle carries the same tolerance in the workflow it owns.
- `scripts/checkout-dependency.sh`, regenerated, understands a dependency
  hosted outside `maelys-dev` and one needing its submodules. This product
  declares neither.

## 0.18.5 — 2026-09-10

- The two reproduction probes of the audit of 2026-09-06 join their report in
  `maelys-dev/maelys-docs`, under `maelys-egress/audits/2026-09-06/`. The
  report moved there in 0.18.4 while its evidence stayed, so its links
  resolved to nothing. They are not tests and were never installed: they
  print observations and assert nothing, and every finding they reproduced is
  closed by a regression test. `docs/` now holds only what this repository
  generates.
- `check-cli-contract` refuses a managed text written by another maelys-cli
  than the pinned one. The framework writes four files here through
  `maelys agents install`, which nothing in this repository regenerates, so a
  pin moved without that command left them behind and no gate noticed. The
  generated CLI and configuration references were already compared; these
  four were not.

## 0.18.4 — 2026-09-09

- The prose documentation moves to `maelys-dev/maelys-docs`, directory
  `maelys-egress/`, with its history: the getting-started guide, the
  configuration and operations references, troubleshooting, the security
  model, the standalone and embedding guides, the lifecycle protocol, the
  TLS notes and the design documents are read there now, and `README.md`
  points at them. The installed package keeps what is generated from this
  repository, the CLI and configuration references and the command
  framework guide, next to the schemas under `protocol/` and the examples.
  Moved by `maelys-release migrate` from the list of
  `maelys-platform docs --prose`, so a document keeps the commits made here.
- CI: a broken third-party apt source of the runner image no longer fails a
  Linux job. The index refresh is best-effort and warns; the install that
  follows decides and still fails loudly when a package is missing. Every
  package this repository installs comes from the Ubuntu archive.

## 0.18.3 — 2026-09-09

- The `egress-cli-contract` skill joins the two others under
  `.claude/skills/`, and the root loses its second skills directory. All
  three address the same reader, an agent changing this repository, and this
  one was neither discovered by Claude Code, which only looks under
  `.claude/skills/`, nor useful where it was shipped: the installed
  documentation carried a procedure naming `cli/main.c` and `make
  cli-reference` to readers who have neither. `AGENTS.md` still links it, so
  Codex reaches it exactly as before.

## 0.18.2 — 2026-09-09

- The fuzz targets and their corpus move from `fuzz/` to `tests/fuzz/`, where
  the other things that verify the product already live. What separates a
  top-level directory here is whether it ships: the library, the command, the
  providers, the headers, the SDKs and the protocol do, the tests and the fuzz
  targets do not. Nothing published changes and no code changes.

## 0.18.1 — 2026-09-09

- The fuzz targets start from a committed seed corpus, 23 inputs under
  `fuzz/corpus/` written by a generator committed beside them so binary
  seeds stay reviewable. Each seed carries the structure its parser looks
  for, valid shapes and the refused ones that sit a byte away, so a campaign
  spends its budget on the boundaries instead of rediscovering that a request
  starts with a method or a TLS record with a handshake byte. libFuzzer reads
  them without writing to the tree; the standalone driver used where
  libFuzzer is absent feeds the same files, and refuses a corpus that is
  missing or empty rather than reporting a run that exercised nothing.
- `public-check` builds every example against the staged install, through
  pkg-config alone, next to the consumer it already ran. The examples ship
  for embedders to copy, and `examples-check` only ever compiled them
  against this tree, which says nothing about whether the installed library
  is enough for them. Verified by making one reach a private header, which
  fails the check.

## 0.18.0 — 2026-09-09

- `make check` runs the conformance kit of `agent-cli-spec` on the built
  binary. The kit drives the command from the outside with read-only
  invocations and checks what the framework owns: the envelope, the exit
  codes, the stream each answer uses and every declared output schema. The
  specification is now a pinned dependency, and `check-spec-contract` refuses
  a pin that is not the version the pinned framework targets, so the two can
  never disagree.
- The local restatement of the agent-cli/v2 envelope schema is gone with the
  check that used it. The envelope is the framework's contract, verified by
  the specification's own kit; `schema-check` now covers only what this
  product owns, the `data` of each command and the daemon's lifecycle lines.
- That validator refuses a schema using a keyword it does not implement,
  instead of skipping the assertion and reporting conformance. Nothing in
  the repository used such a keyword, so the hole was latent.

## 0.17.1 — 2026-09-07

- Three checks borrowed from maelys-oci, whose build proves what this one
  asserted. `public-check` stages the real install, rewrites the prefix of
  every published pkg-config file, then compiles and runs a consumer that
  sees the library only through those files. A dependency the library must
  not carry, or an install that forgets a piece, now fails a link instead of
  passing a grep. `reproducible-check` rebuilds the archive from the same
  objects and compares the bytes, which required making the archive
  deterministic: it is now removed before it is written, so it holds exactly
  its objects, and `ZERO_AR_DATE` keeps timestamps out. The pinned
  dependency checks widen from the contract directories to the whole
  checkout and refuse untracked files, so a stray edit or leftover in a
  dependency can no longer pass unnoticed.
- The library is three layers named by directory. `src/core/` decides on
  bytes alone: parsers, policy, receipts, profiles, attestation, hashing and
  the TLS seam, reaching no descriptor, no clock and no system call. The
  boundary audit refuses any mention of `maelys_sys` there, making permanent
  a property the sources already had. `src/server/` owns the descriptors and
  the reactor behind its own `internal.h`, so membership of that module is
  structural instead of a filename prefix. The durable audit journal, the
  configuration reader and the native connector session stay between them.
  No code changed, only where it lives.
- The static analyzer gate actually fails on a finding. Plain `--analyze`
  reports and exits 0, and `-Werror` does not change that, so the gate was
  passing whatever the analyzer found; `-analyzer-werror` is what fails it.
  Text diagnostics replace the one `.plist` per source that the analyzer was
  writing into the working directory, so the ignore rule for those files and
  its exception both go away.

## 0.17.0 — 2026-09-07

- A `token_file` carrying an embedded NUL is refused instead of yielding the
  bytes before it. The file is read as a bounded byte buffer and then held
  as a C string, so a NUL cut the credential silently: a token of 40 bytes
  whose 21st byte was NUL authenticated on its first 20 bytes, and the
  credential the operator provisioned did not. The minimum length was still
  enforced on the truncated value, so a short prefix failed closed; the
  defect reduced the entropy actually enforced. `audit_key_file` is read as
  raw bytes with its length and was never affected. Found by applying A04's
  reasoning of the audit of 2026-09-06 to the length-prefixed fields its
  sources did not cover.
- Pin maelys-cli `v0.5.19` (`6868bd13cfdccc0cdf59f40174f2bdc76f57bd04`),
  agent-cli/v2 at v2.3.1: the trunk options `--progress`, `--verbose` and
  `--pager` exist on every command and in `globalOptions`, text records in
  a pipe are tab-separated rows, `describe --summary` no longer carries
  `globalOptions`, `invariants` and `output`. The generated CLI reference,
  the framework guide and the agent texts follow. The formula template and
  the contract skill name `dependencies/maelys-cli.pin`, no longer the
  `adapter/` file retired by the 0.14.0 socle adoption.

## 0.16.0 — 2026-09-07

- Pin Maelys System `v0.9.1` (`6663c83a5f6035055b72d3ad0067ac2ad306fc2e`),
  ABI 1 unchanged: a peer's reset seen from the sending side is `ERR_RESET`
  on macOS too, `accept` maps an aborted pending connection to
  `ERR_WOULD_BLOCK` (the listener loop already retries on it), lock and
  trusted opens carry `O_NOCTTY`.
- Adopt maelys-release 0.15.3: the tap publication serializes and retries
  its push, release assets go through a protected draft, a
  `workflow_dispatch` replays the complete flow for an existing tag, and
  the socle's checks run on Ubuntu 26.04. Managed texts unchanged.
- The Python SDK bounds its lifecycle event retention: at most
  `max_pending_events` (default 1024) wait for `next_event()`, the oldest is
  dropped on overflow and `dropped_events` counts it; with `on_event` the
  callback is the consumer and nothing is retained. The stdout reader is
  never blocked. Closes A02 of the audit of 2026-09-06.
- The Python SDK reaches the administration listener over a dedicated
  loopback connection: the proxy environment (`HTTP_PROXY` and variants) is
  never consulted and a redirect is an error, so `health()`, `metrics()`
  and the reload follow-up only ever read the process the SDK started.
  Closes A03 of the audit of 2026-09-06.
- The Python and Node.js SDKs run an automatically discovered binary only
  when nobody but root or the caller could have replaced it: the file, its
  resolution and every directory on both paths are owned by root or the
  caller, never writable by everyone, and group-writable only under the
  caller's ownership; sticky directories are exempt from the write checks. A refused candidate is
  skipped and named in the final error; a privileged process no longer picks
  a binary from a user-owned Homebrew prefix. `binary_trust_refusal()` /
  `binaryTrustRefusal()` expose the rule for explicit paths, which stay
  trusted as given. Closes A01 of the audit of 2026-09-06.
- The security model states the IPv6 admission rule and the deliberate
  refusal of the whole `2001::/23` block, anycast services included, with
  per-destination opt-in as the only exception; the classification tests
  pin `2001:1::1`, `2001:1::2`, `2001:3::1` and `2001:4:112::1` as refused.
  Closes A07 of the audit of 2026-09-06 as a policy decision.
- The HTTP proxy parser refuses DEL anywhere in the header and HTAB in the
  request line, while HTAB inside a field value is still forwarded as
  received (RFC 9112 section 3, RFC 9110 section 5.5). An absolute URI with
  a query but no path is rewritten to origin-form as `/?query`, no longer
  `?query` (RFC 9112 section 3.2.1). Closes A05 and A06 of the audit of
  2026-09-06.
- A TLS `server_name` or a SOCKS5 domain name carrying an embedded NUL is
  refused: the whole length-prefixed field is judged, no longer the prefix
  before the first NUL. Closes A04 of the audit of 2026-09-06.
- Harden request and destination parsing: reject every HTTP header containing
  unpaired CR/LF before forwarding; classify IPv6 from the current global
  unicast allocation with explicit IPv4-mapped and NAT64 handling; and make
  the Python and Node.js process SDKs resolve only fixed absolute executable
  locations. Relative `binary` values and inherited-`PATH` lookup are refused.
- Pin Maelys System `v0.9.0` (`6bd51950c83eaad9ec16cbac318549ab9bb2e928`).
  A peer's reset is `ERR_RESET` again distinct from an orderly close: it
  fails the connection with an I/O result, as before 0.14.0, instead of
  passing as an end of stream. Would-block is the result code
  `ERR_WOULD_BLOCK`, no longer `errno`; the relay, the operations listener
  and the connector test the code. The private connector pair waits with
  `maelys_sys_fd_wait` on each descriptor instead of a private reactor.
  0.8.1's reactor fixes (watch and timer ids never collide, `unwatch` of a
  closed descriptor, no starvation with a small event array, HUP promised
  with READ only) are inherited without change here.
- Adopt maelys-release 0.10.0: the managed texts carry no socle version,
  the provenance attestation follows the repository's visibility, and CI
  calls the socle's `check-product.yml` (checkouts, packages, `make check`
  on the three release targets, drift check) as one job next to the
  product's own; the hand-written drift step is gone.
- Adopt maelys-release 0.13.0 (socle on the Python framework of maelys-cli
  0.5.14, agent-cli-spec 2.2.0): only the two `uses:` lines change.
- Adopt maelys-release 0.14.2 (0.14.1 and 0.14.2 fix the socle's own version
  file and refusal messages; only the two `uses:` lines change here).
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
