# Changelog

## 0.28.3 — 2026-10-07

- **The completion scripts are driven under zsh and fish on Linux.** Neither
  shell was installed on any Linux job, so the conformance kit skipped them
  there, and fish was driven nowhere: macOS has zsh and no fish. `fish` and
  `zsh` are now declared in `dependencies/packages`, which the release
  socle's legs and the release build read, and installed by this
  repository's own Linux jobs (`native`, `sanitizers`, `package`) and its
  test image. Tried before the change reached the CI, in containers of
  Ubuntu 24.04 (fish 3.7.0) and 26.04 (fish 4.2.1): the three scripts offer
  the words of `__complete` and fall back to files, 304 checks passed and
  none skipped.
- maelys-cli **0.5.36** and agent-cli-spec **2.9.0**, the version it targets
  (2.10.0 is not pinned, for the reason given under 0.28.2). Read for runtime
  changes: `MAELYS_CLI_FORMAT` is now the default format in every case, where
  the C library applied it only when no rendering option at all was given.
  With `MAELYS_CLI_FORMAT=json` in the environment, `--compact` answered
  text; it answers compact JSON, and only `--format` and `--json` replace
  the variable. For `serve`, `channel broker` and `channel exec`, which are
  streams, the variable still shapes the failure envelope only. The other
  two rules of 2.9.0 concern commands that write and commands that emit
  records; this product declares neither.

## 0.28.2 — 2026-10-07

- Pins moved to their latest: maelys-cli **0.5.35**, agent-cli-spec
  **2.8.1**, maelys-system **0.12.2**; the release socle was already at its
  latest, 0.62.3. Read for runtime changes, as the adoption rule asks:
  - maelys-system 0.12 adds directory watching (`maelys/sys/dirwatch.h`),
    which Egress does not use, and corrects contracts in its headers; its ABI
    stays 1 and nothing Egress calls changes. `libmaelys_sys.a`, shipped in
    the archives and packages, carries the new object.
  - maelys-cli 0.5.35 changes its Python module only; the C library this
    binary links differs from 0.5.34 by its version number.
  - agent-cli-spec 2.8.1 is the version maelys-cli 0.5.35 targets, which is
    why it is pinned and 2.9.0 is not (`make check-spec-contract`). Since
    2.8.0 the kit drives every bash it finds, the system's included: the
    bash 3.2 of macOS is now held by the gate, where 0.28.1 had measured it
    by hand.

- **0.28.1 did not reach the Homebrew tap; its archives and packages are
  published and correct.** The formula's test expected the manifest to name
  `opt_bin`, the stable path under `opt/`, while `make install` writes the
  path of the keg, under `Cellar/`, and always has: the manifest of an
  installed 0.20.0 names it. The test had never been run on an installed
  bottle. The release socle 0.62.3 runs it on a poured bottle and keeps a
  formula whose test fails out of the tap, which is what happened to 0.28.1
  on both macOS legs; the tap stayed at 0.28.0. Nothing was wrong with what
  the formula installed: the dispatcher accepts that manifest, and its digest
  is the installed binary's. The test now asserts what is true and what
  matters: the manifest names the binary of the keg, and carries that
  binary's `sha256`. A formula is rendered from the tag's own template, so
  the remedy is this release and not a replay of 0.28.1.

## 0.28.1 — 2026-10-05

- **The shell completion `maelys-egress completion` prints did not complete
  under zsh nor under bash 3.2**, the `/bin/bash` of macOS: every release up
  to 0.28.0 offered file names and nothing else. The zsh script read
  `${words[@]:1:CURRENT-1}`, which zsh 5.9 refuses, and the bash script took
  its slice of words after changing `IFS`, which bash 3.2 joins into one
  word. maelys-cli 0.5.34 corrects both, and fish's fallback to paths; a
  script written to a file from an earlier release must be generated again.
  No catalog, option or command of this product changes.
- Pins moved: maelys-cli **0.5.34**, agent-cli-spec **2.7.0**, the release
  socle **0.62.3**; maelys-system was already at its latest, 0.11.0. The
  specification is not at its latest, 2.8.0, on purpose: this repository pins
  the version its framework targets (`make check-spec-contract`), and
  maelys-cli 0.5.34 targets 2.7.0. 2.8.0 comes with the maelys-cli release
  that targets it. Read for runtime changes, as the adoption rule asks:
  maelys-cli changes its library in the completion only (`__complete` no
  longer offers `true`/`false` for an untyped operand, offers a word common
  to two commands once, and never an unavailable command after `help` or
  `describe`); the process functions `channel exec` uses, the trusted files
  and the dispatcher are untouched. agent-cli-spec 2.7.0 makes the kit drive
  each completion script in its shell. The socle asks nothing of this
  repository; at the next release
  the formula's test runs on a poured bottle before the formula reaches the
  tap.

- **maelys-sandbox-policy's network destination corpus is played in the
  tests.** `tests/vectors/policy-destinations` is a verbatim copy of its
  `corpus/destinations` at v0.10.0 (corpus version 1, 29 cases), with a
  `PROVENANCE` file naming the tag, the commit and each file's digest;
  `make policy-corpus-check` refuses a copy that differs. `test-policy-corpus`
  builds each policy through the public calls, seals it with the real sealing
  code and a substituted resolver, since the corpus states what each name
  resolves to, and decides each request with the functions the server uses:
  the host canonicalisation, the sealed lookup and the ClientHello identity
  guard. Result: 28 requests enforced as stated, 3 refused before launch
  (a name that resolves to a private address without
  `allow-private-addresses`, which the contract admits for a mediator that
  resolves when it seals), 20 policies refused at the source (the corpus
  refuses them, or they need `network-host-wildcard`, which Egress does not
  announce), 1 case of `direct` mode, where no mediator is involved.
  Removing the numeric-label rule, the private-address refusal or the SNI
  comparison each makes it report non-conformant cases. It runs in `make test`,
  so every mutant is judged by it too. No code of the product changes.

## 0.28.0 — 2026-10-03

- **A name whose last label is numeric must now be a strict IPv4 literal.**
  `127.1`, `2130706433`, `0x7f.1`, `010.0.0.1` and `1.2.3.04` were taken for
  DNS names and handed to the system resolver, which reads them as addresses,
  and not the same address on every host: `010.0.0.1` is 10.0.0.1 on macOS
  and 8.0.0.1 with glibc and musl, measured. A policy that named one therefore
  did not say where it connected. A name such as `example.0x1` is refused too,
  although no resolver reads it as an address: the rule reserves a numeric
  last label to the strict IPv4 form, so that no name can be mistaken for
  one. A label is numeric when
  it holds decimal digits only, or `0x`/`0X` and at least one hexadecimal
  digit; a name whose last label is numeric is refused unless it is four
  decimal octets of 0 to 255 without a leading zero. The decision no longer
  uses `inet_pton` for IPv4, which accepts `010.0.0.1` on macOS. IPv6 literals
  are kept as they were, exactly as written: the lowering of case applies to
  names only. The grammar is maelys-sandbox-policy's, with its
  fifteen refused and seven accepted names as this repository's test.
- `include/maelys/egress.h` now states what a canonical host is. It did not:
  the parameters were named `canonical_host` and the channel specification
  referred to "the form Egress policies use".
- **Compatibility.** The library revision rises to **5**; the floor stays
  **3**. The names now refused were never within a documented contract: they
  are not IP literals, and as names they meant an address that depended on the
  host. Refusing them corrects an acceptance outside the contract; it does not
  withdraw a behaviour the interface guaranteed, so no declaration of revisions
  3 and 4 stops holding, and both stay served (`tests/public/abi-4.h` is
  frozen from v0.27.1). The revision rises because the header gains the
  definition.
  **Consumer notice**: `maelys_egress_policy_allow_tcp` and
  `maelys_egress_policy_require_tls_sni` return `MAELYS_EGRESS_ERR_ARGUMENT`
  for such a name; a native session and the channel refuse it as a host that
  is not canonical; an HTTP proxy request naming it is malformed, and a
  SOCKS5 domain of that form is refused. A strict private IPv4 is still a
  canonical host, and reaching it is still the private-address rule's
  decision. maelys-warden is not affected for the destinations
  maelys-sandbox-policy now validates by the same rule; that says nothing of
  an older MIR or of another consumer of Egress, which may have named such a
  host and will now be refused.

## 0.27.1 — 2026-10-02

- **The `.rpm` packages shipped a manifest that did not name the binary they
  installed.** `rpmbuild` strips what it packages by default, so the
  installed `maelys-egress` was not the staged one whose `sha256` the
  manifest declares: 371 kB installed against 1.2 MB staged for 0.27.0 on
  arm64. `maelys-egress` itself ran; the `maelys` dispatcher, which runs
  `maelys egress` only when that digest matches, refused it. Every `.rpm`
  published so far has the defect; the `.deb`, the archives and the Homebrew
  bottles do not. The spec now packages the files as staged, and the
  packaging script compares the digest the `.rpm` records for the binary with
  the staged one before it writes the package out.
- New gate: `scripts/check-installed-packages.sh` installs the `.deb` on
  Ubuntu and the `.rpm` on Fedora, each in a clean container, and uses what
  was installed: version, catalog, the manifest's digest against the
  installed binary, the SDK files, and a program started through
  `channel exec`. The package job of the CI runs it on both Linux
  architectures. Until now the packages were listed and never installed;
  the first install, done by hand for 0.27.0, is what found the defect above.

## 0.27.0 — 2026-10-02

- **Stopping the server now ends the connections it relays.**
  `maelys_egress_server_run` closes every connection before it returns,
  proxied tunnels and native sessions included. Until now it closed only the
  opens still pending; a connection already established stayed open and
  carried nothing until `maelys_egress_server_destroy`.
  `maelys_egress_server_stop` may be called from any thread and
  `maelys_egress_server_destroy` only from the owner thread, so a holder
  blocked on such a connection had nothing to wake it: a supervisor that
  stopped its server from a signal thread and waited for its child before
  destroying waited for a child that was waiting for it. `channel exec`
  met exactly that on a mutant. The public header promised the new
  behaviour until 0.26.0 rewrote its comments to describe the old one; the
  comments say it again, and it is now true.
  **Consumer notice**: no declaration changes and no ABI number moves, but
  an embedder that kept using a relayed connection between the return of
  `server_run` and `server_destroy` now reads its end there. Nothing could
  be relayed in that interval, so nothing that worked stops working; the
  `maelys-egress` commands destroyed their server at once and see no
  difference. `tests/test_operations.c` holds a server between the two and
  requires the end of a native session and of a proxied tunnel; the mutant
  `stop-leaves-relayed-open` restores the old condition and is killed.
- `channel exec` no longer destroys its server from the coordinator thread
  when the server stops while the program runs: the stop ends the program's
  connections, and the launcher tears down on the thread that owns the
  server, as the header requires. Its lock and its two-sided teardown are
  removed.
- `tests/test_channel_exec.py`: four assertions that proved less than they
  said are corrected, and the proposal withdraws two tests it listed and
  never carried, with the reason.

## 0.26.0 — 2026-10-02

- New configuration key `channel_exec_by_path` (`true` or `false`, default
  `false`), accepted in exec mode only. `channel exec` executes the program it
  checked through the descriptor it held across the check, so that the object
  executed is the object checked. A multi-call binary that names itself from
  how it was executed then sees `/dev/fd/N` and refuses: the `coreutils` of
  Ubuntu 26.04 does, and `-- /bin/sleep 30` exited at once under 0.25.0. With
  the key set, the checked program is executed through its path, its device
  and inode compared again against the trusted directory immediately before.
  The default does not change and stays the stronger guarantee. The refusal
  of `channel_fd` in broker mode is reworded to name both keys.
- maelys-cli 0.5.33, which adds `exec_by_path` to the options of
  `maelys_cli_process_start` on this repository's report. Read for runtime
  changes: nothing else of the shipped binary depends on it, and the default
  it leaves is the behaviour of 0.5.32.
- `include/maelys/egress.h` said in three places that stopping the server
  ends the connections it relays. It does not, and never did:
  `maelys_egress_server_stop` ends `maelys_egress_server_run` and cancels
  the opens still pending, and it is `maelys_egress_server_destroy` that
  closes the connections already established. Between the two they stay
  open and carry nothing, so a holder blocked on one waits for the destroy.
  The comments now say so. No declaration and no behaviour changes, and no
  ABI number moves; `channel exec` found it and destroys its server at once
  for that reason. An embedder that stops a server and keeps it should
  destroy it as soon as `server_run` has returned.
- The README states two limits of `channel exec`. `channel_fd = 3` is valid
  and is not what a program written for maelys-warden expects, since
  descriptor 3 carries Warden's own protocol there. And a multi-call binary
  that names itself from how it was executed, as the `coreutils` of Ubuntu
  26.04 does, does not start: the program is executed through the checked
  descriptor, not through its path.

## 0.25.0 — 2026-10-02

- **New command `maelys-egress channel exec --config FILE -- /absolute/program
  [ARGUMENT...]`.** It starts one program that speaks the mediated-connection
  channel and hands it the channel as an inherited descriptor, the way
  maelys-warden does on descriptor 4: a server with no listener, one channel,
  one program, and that program's exit status. No socket on disk, no port, no
  secret. It is what `examples/channel_supervisor.c` shows, as a command.
  - The channel is on the descriptor `channel_fd` names, **4** by default,
    announced in `MAELYS_EGRESS_CHANNEL_FD`;
    `MAELYS_EGRESS_CHANNEL_CONNECT_TIMEOUT_MS` carries
    `channel_connect_timeout_ms`. The rest of the environment is inherited.
  - Standard input, output and error are the program's. The command writes no
    lifecycle record; receipts go to `audit_log` only.
  - The exit status is the program's, `128 + signal` when a signal ended it,
    as agent-cli/v2 requires of a stream command. A JSON error envelope on
    stderr with exit 1 means the program never started: once it has, nothing
    Egress does replaces its status.
  - `SIGINT` and `SIGTERM` are forwarded to the program and the server stops
    only after the program has exited; `SIGHUP` reloads the destinations and
    reports on stderr.
  - **It is not a sandbox**: the program keeps its own network access.
  The contract and its reasons are in `proposals/egress-channel-exec.md`.
- **Configuration: a third mode, and a file that was refused is now valid.**
  `channel_principal` without `channel_listen_unix` is the mode of
  `channel exec`; until now `config validate` refused it. New key `channel_fd`
  (3..255, default 4), accepted in that mode only.
  `channel_handshake_timeout_ms` and `channel_max_clients` remain the
  broker's and are refused without `channel_listen_unix`. Each of `serve`,
  `channel broker` and `channel exec` refuses a file of another mode.
  `config describe` changes accordingly: `channel_principal` no longer
  requires `channel_listen_unix`, the proxy keys conflict with
  `channel_principal` instead of `channel_listen_unix`, and three constraint
  sentences are reworded. A consumer that compares those strings must update.
- `examples/channel_client.c` reads the descriptor number from
  `MAELYS_EGRESS_CHANNEL_FD` instead of its first argument, and
  `examples/channel_supervisor.c` sets it: one convention for the example,
  the command and a program written for either.
- maelys-cli 0.5.32. 0.5.31 added `maelys_cli_process_start`, `_signal`,
  `_wait` and `_release` with a mapping of inherited descriptors, and
  `maelys_cli_environment_to_envp_inherited`. They were asked for by this
  command; nothing else of the shipped binary depends on them. 0.5.32
  corrects a data race this command found in them under ThreadSanitizer:
  the handle's `exited` flag, written by `wait` and read by `signal` from
  another thread as the contract allows, is now atomic. Read for runtime
  changes, as the adoption rule asks: 0.5.31 also makes the `maelys`
  dispatcher trust a manifest by the directory it resolves to instead of
  refusing a symbolic link, which is what lets it read the manifest Homebrew
  links from its cellar, `share/maelys/commands/egress.json` included. That
  is the dispatcher's behaviour, not this binary's: `maelys-egress` reads its
  configuration with the requirements it had, and nothing it ships changes
  for that reason. 0.5.32 makes an extension the dispatcher cannot run
  unavailable instead of stopping the dispatcher. The agent texts are
  refreshed with `maelys agents install . --apply` from the pinned checkout.
- The tag `v0.24.0` no longer exists. The 0.24.1 entry below says it "stays
  where it is": it was removed afterwards, by decision of the maintainer,
  and is not recreated. It had published nothing, but GitHub served a source
  archive for it whose `VERSION` read 0.23.0 while its headers already
  carried the new ABI numbers; left in place, it invited a pin on an
  incoherent tree. The rule that a published tag is never moved stands: a
  tag that changes commit deceives whoever fetched it, and this one was
  deleted, not moved. A clone that fetched it still has it locally.
- `make abi-floor-check` now holds every revision an interface serves, not
  its floor alone, following an amendment of the family's ABI policy: the
  declarations of the floor do not show the removal of a function that a
  later revision added. `tools/check_abi_fragments.py` requires one frozen
  fragment per revision from `..._ABI_COMPATIBLE_SINCE` to
  `..._ABI_VERSION - 1` and refuses one below the floor. The four fragments
  are regenerated from the same tags with names that carry the fragment's
  own, so that two revisions of one interface compile together; what they
  assert is unchanged. Nothing changes for a consumer: no header, no
  number, no shipped file.

## 0.24.1 — 2026-10-01

- 0.24.0 was never published; this release carries everything its entry
  below describes. The signed tag `v0.24.0` was placed on the commit before
  the version change, where `VERSION` still read 0.23.0, and the release
  workflow refused it at its first step: no archive, no package and no
  formula exist under that name. A published tag is never moved or
  recreated, so `v0.24.0` stays where it is and must not be pinned. The tag
  was cut by a script that did not check, after merging the version, that
  the commit it was about to sign was the merged one.

## 0.24.0 — 2026-10-01

- Consumer notice: the interface is now numbered twice.
  `MAELYS_EGRESS_ABI_VERSION` is its revision and becomes **4**;
  `MAELYS_EGRESS_ABI_COMPATIBLE_SINCE`, new, is **3**. For the client,
  `MAELYS_EGRESS_CLIENT_ABI_VERSION` becomes **2** and
  `MAELYS_EGRESS_CLIENT_ABI_COMPATIBLE_SINCE` is **1**. Nothing is removed or
  changed: revision 4 is the interface of 0.23.0, which had grown from 86 to
  93 functions under the number 3, and client revision 2 is the client of
  0.23.0. A consumer written for revision N is served when
  `COMPATIBLE_SINCE <= N <= ABI_VERSION`: the lower bound says nothing it
  uses was broken, the upper bound that what it uses exists. A consumer that
  tests `MAELYS_EGRESS_ABI_VERSION == 3u`, as maelys-warden does on its
  pinned checkout, must move to `4u` when it moves its pin, or test the
  interval against an installed library. `maelys_egress_abi_compatible_since()`
  and its client twin return the floor the library was built with. The two
  module interfaces get their second number too, both at their revision:
  `MAELYS_EGRESS_TLS_ABI_COMPATIBLE_SINCE` and
  `MAELYS_EGRESS_TLS_FILES_ABI_COMPATIBLE_SINCE` are 1.

- Each floor is held by the compiler. `tests/public/abi-3.h`,
  `abi-tls-1.h`, `abi-tls-files-1.h` and `abi-client-1.h`, one per interface
  and named after the floor it holds, are generated by `tools/freeze_abi.py`
  from the tags of the floor revisions (v0.20.0, and v0.22.0 for the
  client): 131 declarations made again against the current headers, 81
  static assertions on every enumerator, numeric macro and open-structure
  layout, and a first pass that requires the current headers to still name
  each function and type, since a declaration made again would hide a
  removal. The two numbers themselves are not frozen: the revision is meant
  to rise. `make abi-floor-check`,
  part of `make check`, was seen to refuse a changed signature, a moved
  enumerator, a reordered open structure, a changed wire macro and a removed
  function, and to accept an added function and an added enumerator. The
  rule is maelys-platform's ABI policy, adopted for the whole family on
  2026-10-01; `AGENTS.md` states how this repository applies it.

- Consumer notice, late: 0.23.0 added `MAELYS_EGRESS_CLIENT_ERR_BUSY` to
  `maelys_egress_client_result_t`. No existing value moved, but a consumer
  that switches over this enumeration without a default, under
  `-Werror=switch`, stopped compiling; maelys-warden did. Only
  `maelys_egress_client_channel_open` returns it. `maelys_egress_client_connect`
  never does: a status it does not know is `ERR_INTERNAL`, with no stream.
  0.23.0 said none of this.

- The public headers now say that their enumerations may gain enumerators,
  that a switch needs a default which fails closed, and which results each
  client call can return. A frozen consumer, `tests/public/frozen_results.c`,
  names every enumerator of the three public result types as of 0.23.0 with
  no default and is compiled under `-Werror=switch` by
  `make consumer-source-check`, part of `make check`: it fails when an
  enumerator is added, so the next addition is a decision written in this
  changelog and not a surprise for a consumer. The client test also holds
  the fallback: a result or a wire status this build does not know is a
  failure with nothing usable, never success. `AGENTS.md` states the rule.

## 0.23.0 — 2026-09-30

- Freeze the implemented channel bootstrap v1 contract for publication under
  `protocol/egress-channel-bootstrap-v1.md`, following the independent review
  and complete Linux/macOS and Compose gates. Preserve the design and all
  17 evidence criteria in the accepted proposal. Remove the experimental
  label from the CLI catalog, generated references, headers and example;
  retain core ABI 3 and client ABI 1 without changing wire bytes or runtime
  behavior. Verify installation of both native protocol documents in the
  same temporary staging tree.

- Complete the bootstrap pre-freeze test evidence: forced short reads and
  control-truncation injection, absolute trickle deadlines, post-creation
  delivery failures with ownership/join/slot recovery, control-flood fairness,
  non-reading peers, process death and owner/ancestor-symlink refusals. Add
  bootstrap-only mutation and seeded fuzz gates to a required macOS job,
  alongside Linux, and retain the 17-criterion checklist in the proposal.

- Add the native Compose example and required Docker CI gate.
  A distinct-UID, networkless application links only the client archive and
  obtains its channel through a read-only socket volume without credentials.
  A local fixture proves allowed mediation, same-target direct-network
  failure, explicit policy refusal, stream lifetime and identity receipts.
  The test checks live Docker state and cleans only its private project.

- Add `maelys-egress channel broker --config FILE`: a native-only server and
  supervised Unix-path broker, with immutable secret-free principal binding,
  bounded clients/deadlines, quotas, audit and policy-only SIGHUP reload.
  Six channel configuration keys select this mode; proxy/credential/TLS
  listener keys and the wrong command are refused. `ready.channel` replaces
  `ready.proxy` in broker mode only. Receipts cannot precede `ready`, and a
  failed broker stops the command with a fatal event and nonzero exit.
  Add trusted native-principal/connector and broker liveness APIs within ABI 3.
  Cross-mode, standalone-client, identity, quota, reload and lifecycle tests
  accompany generated references. Permanent shutdown tests keep four leases,
  four streams and a confirmed partial handshake alive across 20 signal stops
  and a broker failure, checking receipts, closure and socket cleanup.
  Python/Node process SDKs remain proxy-only
  and reject channel readiness.

- Add the Unix-path channel bootstrap library and standalone
  client handle. A private socket authorizes channels from one pre-bound
  connector; the client retains a lifetime lease and needs no Egress core,
  thread runtime or proxy credential. A separate broker worker owns bounded
  handshakes, descriptor delivery and leases. A second, fixed cleanup worker
  joins retired channels without blocking either reactor; retiring channels
  retain their capacity slot until fully destroyed. Parent
  ownership/modes and identity-checked socket cleanup enforce the filesystem
  capability boundary. ABI numbers stay 3 (core) and 1 (client).
- Add byte vectors, fragmented/partial I/O and ancillary-data attacks,
  capacity/deadline/cleanup regressions and a suspended-open test proving
  broker/proxy progress, handshake expiry and lease handling during channel
  destruction, with bounded saturation and full capacity recovery. A mutation
  restores synchronous destruction and must fail the broker progress test.

## 0.22.5 — 2026-09-30

- Adopt maelys-system 0.11.0 and harden the optional mbedTLS and wolfSSL
  transports on Unix stream sockets. Their shared receive adapter drains
  ancillary data through System's fdpass, closes every attached descriptor
  and permanently rejects the TLS session on unexpected control data,
  including macOS control-only messages. Other stream families retain their
  byte-only transport, including AF_VSOCK as required by the TLS seam;
  providers still borrow sockets without closing or changing their mode.
  This closes the optional TLS-over-Unix gap documented in 0.22.4.
- Regression tests inject descriptors into genuine TLS handshake and data
  records for both roles and both providers, check descriptor counts and
  failed-session retries, and exercise ordinary Unix/TCP exchanges. The
  transport tests run without optional TLS dependencies in the default gate.
  The boundary audit now also refuses native descriptor-passing calls in
  providers and direct reads outside their shared transport adapter.
- The security regression gate now checks 128 repeated Unix attacks with
  queued descriptor transfers, rights attached at four TLS record boundaries,
  maximum-size descriptor batches, and continued traffic on a healthy session
  sharing the attacked provider. Provider mutation tests restore the original
  raw Unix receive in each TLS stack and require an actual assertion failure
  after a passing unmodified control. The default mutation gate also restores
  the former stream-family restriction to prove its regression test detects it.

## 0.22.4 — 2026-09-29

- Adopt maelys-system 0.10.1 and rebuild the statically linked proxy. Its
  byte-only Unix socket receive closes attached SCM_RIGHTS instead of letting
  macOS install them silently. This fixes descriptor exhaustion on the plain
  Unix proxy listener before authentication and during relay. The integration
  test attaches fifty descriptors to refused and accepted HTTP requests,
  relayed data and a SOCKS greeting, and checks the final descriptor count.
  The optional TLS-provider callbacks use separate I/O and are not covered
  by this byte-only correction; do not expose TLS-over-Unix to untrusted peers
  until that path is hardened too. TCP listeners do not carry SCM_RIGHTS.
- Adopt maelys-release 0.62.2 without runtime changes from the socle. The
  release hook keeps the shipped Python and Node SDK versions and install
  examples synchronized with VERSION.

- A runnable Docker Compose example gives an ordinary proxy-aware application
  no ambient network while Egress keeps the only outbound route. A bridge in
  the application's network namespace exposes authenticated HTTP proxying on
  loopback and relays it to Egress's private Unix socket; deployment-time
  initialization creates owner-only token copies for the two identities and
  materializes an Egress-owned configuration instead of trusting the host UID
  of a bind mount. The integration gate proves that direct access fails, the
  allowed request succeeds through Egress, and the application and bridge have
  exactly the intended Docker network modes. The example is installed with
  the other documentation.

## 0.22.3 — 2026-09-29

- The native channel server cannot be held inside `sendmsg` by a confined
  client that sends requests without reading their answers. Its end of the
  datagram pair is nonblocking; when the response queue is full, it closes the
  stream it could not deliver and ends the channel. The worker closes its own
  channel end on every exit, so the client observes the end and
  `maelys_egress_channel_destroy` can always join it. An adversarial operations
  test fills the response queue, keeps the client end open and requires
  destruction to return within 300 ms; the old Linux implementation blocked
  until the client descriptor was closed.

- The standalone channel client's read deadline is one monotonic interval
  across interruptions. `poll` used the complete `read_timeout_ms` again
  after every `EINTR`, so a process receiving signals could wait without
  bound. The client now subtracts elapsed monotonic time after each wake and
  splits only deadlines larger than `INT_MAX`; a test interrupts an 80 ms
  wait every 5 ms and bounds the whole call, with a parent process that turns
  the former hang into a deterministic failure.

- Adopt maelys-release 0.62.1, which asks nothing of a product: two workflow
  pins follow.

## 0.22.2 — 2026-09-28

- Descriptors cross the channel through maelys-system's `fdpass`, adopted
  with maelys-system 0.10.0 on both sides. The server and the client each
  carried their own `SCM_RIGHTS` code, and the two copies had drifted: one
  of them was the flaw of 0.22.0. `maelys_sys_fd_send` and
  `maelys_sys_fd_receive` now do it once, with room for every descriptor
  either kernel delivers, the surplus closed and flagged, close-on-exec set;
  the server asks for no descriptor and refuses a request that brought some,
  the client asks for one. No `sendmsg` or `recvmsg` remains in Egress, and
  the boundary audit refuses them everywhere, where 0.22.0 named one file as
  an exception.

- The client's rule changes, with maelys-warden's agreement, since the rule
  was Warden's: from "no maelys-system" to "`fdpass` alone, and no undefined
  `maelys_sys_` or `pthread_` symbol in the archive". The motive is one
  proven copy of descriptor passing instead of two that drift. The archive
  holds `fdpass.o` taken from the pinned `libmaelys_sys.a` — the very object
  the library holds, which also works against an installed maelys-system
  that ships no source. `client-standalone-check`, part of `make check`,
  refuses any `maelys_sys_` or `pthread_` symbol an object of the archive
  needs and another does not define, and was seen to refuse an archive
  without `fdpass.o` and one with a `pthread` call; the archive still links
  alone in the client test, the release smoke and the Homebrew test; the
  audit keeps `client/` to `fdpass.h` and its two functions. The contract
  records the change and its reason.

## 0.22.1 — 2026-09-28

- The channel server closes every descriptor a request carries, and refuses
  the request. It received requests without a control buffer, on the belief
  that the kernel drops descriptors the receiver has no room for. Linux does,
  and says so with `MSG_CTRUNC`; macOS installs them in the receiving process
  regardless and says nothing — measured with 1, 50 and 200 descriptors. On
  macOS a confined process could therefore attach up to 254 descriptors to
  each request and fill the descriptor table of the process that mediates its
  network: a denial of service on the channel of 0.22.0. Found by
  maelys-system while measuring both kernels for its descriptor-passing
  primitive. The server now reads with room for every descriptor a kernel can
  deliver in one datagram (254 on macOS, 253 on Linux), closes them all, and
  answers `MALFORMED`; the contract states the rule. The client, whose room
  was eight descriptors, gets the same room and reads no further than the
  control bytes the kernel filled: on macOS a truncated header can announce
  more than was delivered, and the client would have closed integers read
  past its buffer. Its peer is the trusted server, so that half is
  robustness. Tests send fifty descriptors with a valid request and twenty
  with an `OK` response, and count descriptors around each; both failed
  before the change on macOS. The mutation gate holds the refusal.

## 0.22.0 — 2026-09-28

- The mediated-connection channel, version 1, is a contract:
  `protocol/egress-channel-v1.md`, installed with the other protocol
  documents, replaces the proposal of 0.21.0 and records maelys-warden's
  answers to its four open questions. Two examples are its two sides:
  `channel_supervisor.c` runs Egress with no port, binds a channel to a
  connector and hands its client end to a child on a descriptor number of its
  choosing, started by absolute path; `channel_client.c` links
  `libmaelys_egress_client` alone and asks for one destination. Without
  arguments the supervisor serves a loopback echo, and `examples-check` runs
  the round trip with no network; `public-check` builds the client example
  against the installed client archive through its own pkg-config file. The
  criterion the proposal set is met: an integrator uses the native mode
  across a process boundary without importing maelys-warden or reading its
  sources.

- `examples/native_connector.c` creates and destroys its server on the
  thread that runs it. `maelys_egress_server_run` and
  `maelys_egress_server_destroy` belong to the thread that created the
  server — the reactor is bound to it — and the example created it on the
  main thread, so its run failed at the first reactor step with "reactor
  step failed", and a destroy from the main thread would have left the
  reactor allocated; nothing ran it, since it reaches example.com. Found
  while writing `channel_supervisor.c` from it, which `examples-check` and
  the sanitizers do run.

- The channel server, third part of `protocol/egress-channel-v1.md`.
  `maelys_egress_channel_create` binds a datagram pair to an authenticated
  connector and returns the client end for the supervisor to hand over; a
  thread serves it, one request at a time and in order, until
  `maelys_egress_channel_destroy`. Each request becomes one
  `session_open` of the connector under the deadline the creator chose, and
  the answer carries the stream as the one passed descriptor, or a status
  from the protocol's explicit table: the policy's refusal is `DENIED`, a
  server that is stopping or has stopped is `CANCELLED`, a host the codec
  accepts but the connector refuses as not canonical is `MALFORMED`. The
  server closes its copy of the stream and releases the session whether or
  not the kernel took the datagram; destroying the channel touches no stream
  already handed over. `src/channel_server.c` is the one file allowed to
  call `sendmsg` and `recvmsg`, for the descriptor: maelys-system has no
  primitive for `SCM_RIGHTS` yet, and the boundary audit names the exception.
  The operations test drives both ends in one process — the client archive
  on one side, the library on the other, raw datagrams where the client
  would never produce the case — and counts descriptors around every
  exchange; the mutation gate holds the `DENIED` row of the table.

- `libmaelys_egress_client`, the channel client a confined process links,
  second part of `protocol/egress-channel-v1.md`. One call,
  `maelys_egress_client_connect`, sends a request on the channel the
  supervisor handed over and receives the relayed stream: exactly one
  descriptor on OK, none otherwise, `CLOEXEC` set on receipt, a read deadline
  that shuts the channel rather than leave a late answer to pair with the
  next request, protocol-owned status codes mapped to the client's results,
  and every descriptor a server in breach might send closed. The archive
  holds `client/client.c` and the codec and stands on the C library: its
  test, the release smoke and the Homebrew test link it without maelys-system
  and without `-pthread`, and the boundary audit keeps `client/` from naming
  either. Installed with `maelys-egress-client.pc`, which requires nothing.
  ABI 1. Nothing serves the channel yet.

- The codec of the mediated-connection channel, first part of
  `protocol/egress-channel-v1.md`. `include/maelys/egress_channel.h` names
  the bytes of version 1 — the request header and its host, the 8-byte
  response, the eight status codes of the protocol — and `src/core/channel.c`
  encodes and decodes them with the C library alone, so the same object can
  go into the server library and, next, into the client archive a confined
  process links. Twenty-five request vectors and seventeen response vectors
  under `tests/vectors/channel/` state what the codec accepts and refuses;
  `test-channel` replays them against the codec object alone and round-trips
  every accepted one, a fuzz target does the same on every input, and the
  mutation gate holds the host bound. Nothing speaks the protocol yet: no
  server, no client, no descriptor crosses a process in this release.

## 0.21.0 — 2026-09-28

- The SNI guard holds on every protocol. A destination allowed with
  `allow_tls_sni` admits only a TLS ClientHello that names it — but an HTTP
  forward request to the same host and port was relayed as cleartext, since
  the guard ran on tunnels only and nothing turned the forward mode off; the
  header even said so. Found by maelys-warden while wiring the flag through its
  adapter. Such a request is now refused as `DENIED` at admission, before any
  upstream connection, and the receipt records it with protocol
  `HTTP_FORWARD` and `tls_sni_verified` 0. The header and the key's description
  say it; a test sends the request and reads the refusal and the receipt.

- A proposal for the mediated-connection channel, `protocol/egress-channel-v1.md`:
  the interprocess channel to the native connector — today maelys-warden's
  private pair, `fd4_broker` and `netclient` — would belong to Egress, with a
  versioned protocol, the broker, a client a confined process links without
  the Egress core, conformance vectors and adversarial tests, while the
  supervisor keeps process creation, identity binding, the descriptor it hands
  over and the removal of other network paths. Not implemented, not a
  contract: v1 freezes after the implementation and its gate. maelys-warden
  has answered its four open questions and accepted the boundary. The README's
  CLI section now names the native connector and what it is for.

- Adopt maelys-release 0.62.0. 0.61.0 asks a product that pins other Maelys
  repositories to re-adopt: `scripts/checkout-dependency.sh` gains the bundle
  path a runner without credentials uses for a private pin, and this
  repository carries none, so the file changes and nothing else does. 0.62.0
  asks nothing of a product. Two workflow pins follow.

- `tools/check_public_docs.py` no longer looks for the name of a private
  documentation repository, so the repository no longer carries that name
  anywhere: the socle's `check` refuses it in the seeded texts since 0.61.0,
  and the four public files that once carried it were cleaned in 0.19.13.
  The tool keeps its two other rules, local links that exist and prose that
  does not end on a dangling word, over the same nine files.

## 0.20.0 — 2026-09-25

- `maelys_egress_tls_files_t` states its layout. The structure a caller fills
  for `maelys_egress_tls_mbedtls_create` and `maelys_egress_tls_wolfssl_create`
  was the one open type without a version: a field added to it could be read
  from a caller that never wrote it. It now opens with `abi_version`, set to
  `MAELYS_EGRESS_TLS_FILES_ABI_VERSION` (1), and both factories refuse any other
  value with `MAELYS_EGRESS_ERR_UNSUPPORTED` and a diagnostic naming both
  versions, as `maelys_egress_tls_provider_create` already does for the provider
  table. The constant is distinct from `MAELYS_EGRESS_TLS_ABI_VERSION`: a change
  to one structure leaves the other valid. The layout change makes this Egress
  ABI 3; every other type, function and behavior of ABI 2 is unchanged. Done
  before 1.0, when it is the last open structure of the public headers.

## 0.19.14 — 2026-09-24

- The test image retries a package fetch. `apt-get` runs with
  `Acquire::Retries=3`, for update and for install: this release was held by a
  single unanswered fetch from `security.ubuntu.com`, on the one step that had
  no retry where the workflows installing packages already tolerate a failing
  source.

- Adopt maelys-cli 0.5.30, and take the built digest after each install rather
  than once before them. The framework bakes
  `-DMAELYS_CLI_COMMANDS_DIR=PREFIX/share/maelys/commands` into its objects, so
  the binary built for one prefix is not the binary of another;
  `install-metadata-check` had asserted one digest against both installs since
  it was written. Nothing contradicted it while a prefix change rebuilt
  nothing, and 0.5.30 rebuilds on it. The framework targets the same
  agent-cli-spec 2.6.0, conformance stays at 267 checks, and neither the
  product code nor `docs/cli-contract.json` moves.

- A build directory remembers the command line it was made with and drops its
  objects and binaries when that line changes. Measured before the change:
  `make CC=gcc` over a tree built with `cc` rebuilt nothing at all, and `make
  check CC=gcc` then reported success over the objects `cc` had produced;
  `CFLAGS='-O0 -g'` likewise left an `-O2` binary in place. A diagnostic only
  one compiler emits could stay invisible until CI, which is how maelys-cli's
  signed-`char` defect was caught twice by an x86 leg and never on its own
  machine. The comparison happens while the makefile is read, not through a
  stamp every rule depends on: the make of macOS is 3.81 and compares
  modification times to the second. `make -n` writes nothing and `make clean`
  triggers no removal.

## 0.19.13 — 2026-09-24

- The unset-dependency guard says "the lines it prints": since the socle 0.60.0
  adoption, `scripts/checkout-dependencies.sh` prints `MAELYS_RELEASE_DIR`
  beside `MAELYS_DEPENDENCIES_DIR`, and someone exporting only the first laid
  down half a root. The CI was never affected; its jobs append the whole output.

- Compile the command-line objects against the pinned maelys-cli headers before
  any ambient `CPPFLAGS` include directory. A machine with Homebrew's older
  `libmaelys-cli` installed could otherwise compile older public structure
  layouts and link the pinned archive, producing an immediate segmentation
  fault even though a clean CI runner passed.

- Repair the public documentation after the prose migration: complete the TLS
  section, rebuild the documentation map from files that actually ship, remove
  stale product and dependency version claims, document the isolated dependency
  root and current release procedure, and remove the remaining private
  documentation-repository name from an installed example guide. A new
  `docs-check` gate rejects broken local Markdown links, that private name and
  prose paragraphs ending in a dangling connector.

- Adopt maelys-release 0.60.0. The three former image-named CI aliases are no
  longer emitted, after the fleet-wide migration to platform-named legs. This
  repository already requires none of the old names, so the adoption changes
  only the three shared-workflow pins and removes three redundant jobs from
  each pull request. The 0.59.2 impact asks nothing of this product.

- Adopt maelys-release 0.59.1. Two workflow pins, nothing asked. The socle
  now reads a branch's protection, a repository's metadata and its own
  declaration words through one reader each, where four, seven and three sites
  had read them separately. It announces that 0.60.0 removes the leg aliases
  kept since 0.57.0; `main` here requires none of the old names, so nothing
  will block.


- Adopt maelys-release 0.57.1. Two workflow pins. Among its fixes,
  `protect --apply` no longer rewrites a whole classic protection to change its
  checks: it had reset "require branches to be up to date" elsewhere. This
  repository ran `protect --apply` under 0.54.0, so its protection was compared
  setting by setting with a reading taken before the rename, and nothing had
  changed — that setting was already off here.

- Adopt maelys-release 0.57.0. A rename of the shared CI's legs no longer
  narrows a branch protection: the socle reports the former names as aliases,
  each red when any leg is red and run under `always()`, so an adoption removes
  nothing a branch requires and `protect --apply` swaps each alias for its leg
  in one write. This repository had already completed the 0.54.0 rename, so
  nothing is asked of it; three short alias jobs run on each pull request until
  a later version removes them.

- Adopt maelys-release 0.56.0. The adoption marks each change by what it
  touches, and here that is two workflow pins and three agent texts, no
  mechanism. Those texts carried three false statements that 0.55.0
  corrected, among them "public repositories use GitHub-hosted runners only",
  false since 0.53.0, and an incomplete rename order that omitted the merge
  between adopting and widening. This repository had widened after merging,
  but by caution rather than because the text said so.

- Adopt maelys-release 0.54.0, whose shared CI legs are named after a
  platform rather than a runner image: `check (linux)`, `check (linux-arm64)`
  and `check (macos)` replace names carrying an Ubuntu or macOS version. A
  required context that names an image turned every image upgrade into a lock
  on every protected branch. The rename is survivable in one order only, which
  this adoption followed: the three old names were removed from `main`'s
  required checks first, the socle adopted, and the new names added back once
  they had run.

- Adopt maelys-cli 0.5.29 and agent-cli-spec 2.6.0, the specification the
  framework targets. 2.6.0 lets an operand declare `algorithms`, `digits` and
  `pattern`, all optional, and requires nothing of an implementation: the
  conformance kit stays at 267 checks, all passing, and neither the product
  code nor `docs/cli-contract.json` changes.

- Adopt maelys-cli 0.5.27 and agent-cli-spec 2.5.0. The framework's first tag
  to read its pinned dependencies under one root, which closes the cascade this
  repository reported: built without a root it now stops on one message naming
  the command to run, instead of a missing file three levels down. The
  conformance kit goes from 259 to 267 checks, all passing, with no product
  code changed and no movement in `docs/cli-contract.json` — the specification
  bump adds rules about how the specification itself is written and what a
  framework owes the kit, not obligations a product must meet.

- `maelys-release.conf` declares `[gate] none`: the `release` environment
  requires no reviewer, and that is now written down rather than left tacit.
  `preflight` used to note the absence on every release; it now reports it as
  a choice the repository states. Nothing changes about how a release runs —
  only whether the repository says so.

## 0.19.12 — 2026-09-14

- Adopt maelys-release 0.50.1. Only the two workflows change, and `check`
  reports no note at all: its search for a build that reaches next door now
  reads every tracked file rather than `ci.yml` alone, so it can confirm what
  it could not before — the three fixes of 0.19.11, in the test image and two
  scripts, left nothing behind. The advice on the Homebrew renderer became a
  statement of evidence, naming `archive/refs/tags` in our script as the
  reason it hashes bytes it downloaded rather than an archive a macOS runner
  would rebuild.

## 0.19.11 — 2026-09-13

- Adopt maelys-release 0.46.1 and read every pinned dependency under one root.
  `maelys-release.conf` declares `[dependencies] apart`; the socle then
  materialises the pins away from this repository and exports
  `MAELYS_DEPENDENCIES_DIR` in each place it clones, and the new managed
  `scripts/checkout-dependencies.sh DESTINATION` does the same for a developer
  in one command. The Makefile reads `$(MAELYS_DEPENDENCIES_DIR)/NAME` and no
  longer falls back to a sibling: a directory beside this one cannot be told
  apart from the working copy of someone who develops that dependency too, and
  this build lost four `make check` runs in a day to exactly that. Naming a
  single `MAELYS_*_DIR` by hand still works; only the case where nothing says
  where anything is now fails, with our own message instead of a silent read.
  The five hand-written CI jobs clone once each instead of three times, and the
  release workflow does the same. Three places of this repository assumed a
  sibling of their own accord and had to follow: `docker/Dockerfile.test`,
  which cloned into the image and then built with nothing set, and
  `scripts/check-installed-system.sh` and `scripts/mutation-check.sh`, which
  carried a hard-coded `../maelys-system` fallback that ignored the root the
  environment already held.

## 0.19.10 — 2026-09-12

- Adopt maelys-release 0.41.0 and maelys-cli 0.5.25. Only the workflow pins
  and the managed agent texts change here; neither asks anything of this
  product. The framework's own maelys-json moves to 0.2.0, which removes two
  functions nothing here calls — this product does not depend on maelys-json.
  The socle brings a `[runners]` declaration for its macOS jobs, honoured on a
  private repository only, a `cut` that audits its own write before creating a
  branch, an SBOM attested against the file it describes, and a check that a
  version carrier has not stopped carrying the version.

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

- `check-cli-contract` refuses a managed text written by another maelys-cli
  than the pinned one. The framework writes four files here through
  `maelys agents install`, which nothing in this repository regenerates, so a
  pin moved without that command left them behind and no gate noticed. The
  generated CLI and configuration references were already compared; these
  four were not.

## 0.18.4 — 2026-09-09

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
