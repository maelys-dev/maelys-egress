# Maelys Egress channel bootstrap, v1

**Status: accepted and implemented — bootstrap v1 frozen for 0.23.0.**
The normative contract is now
[`protocol/egress-channel-bootstrap-v1.md`](../protocol/egress-channel-bootstrap-v1.md).
This document retains the design history and durable 17-criterion evidence
checklist; its original proposal wording below is not a second contract.
The mediated-connection channel itself remains the unchanged
[`egress-channel-v1`](../protocol/egress-channel-v1.md) contract.

The independent pre-freeze review of PR #149 at `160b125` found no blocking
issue and verified all required checks (18 passed, one intentionally skipped),
including the required native Compose test, Linux/macOS bootstrap mutation
and fuzz gates. The tests merged without a tag as `ea30a40`. The two mistaken
shutdown-test filenames noted in that review are corrected below. The freeze
changes no runtime logic or wire bytes and keeps ABI 3 (core) and 1 (client).

## Why

`maelys_egress_channel_create` returns an anonymous `SOCK_DGRAM` socket-pair
end. A supervisor that creates the workload can hand that end over before
`exec`, as [`channel_supervisor.c`](../examples/channel_supervisor.c) does. A
Docker Compose service starts through the Docker daemon instead: no descriptor
inheritance path crosses that boundary, and the frozen channel deliberately
defines no discovery by pathname.

The missing gesture is small but security-sensitive: a workload connects to a
private filesystem socket, asks for channel version 1, receives the client end
with `SCM_RIGHTS`, and keeps the bootstrap connection open as the channel's
lease. The target criterion is:

> An Egress-aware application in a separate container with
> `network_mode: none` obtains and uses a native mediated-connection channel
> without Warden, an inherited descriptor or a proxy credential.

This is not HTTP `CONNECT`, SOCKS5, a new form of the channel messages, or a
way to inject descriptor 4 into an already running process.

## Boundary

Egress owns:

- this versioned bootstrap protocol;
- an independently embeddable broker with a reactor for the listener and
  leases, and a separate bounded cleanup worker for channel destruction,
  bound at creation to one execution principal;
- the client open operation in `libmaelys_egress_client`;
- the lifetime lease joining a broker-owned channel to its remote holder;
- the codec, vectors, cross-platform conformance and adversarial tests;
- a Compose example whose application has no network and no Egress secret.

The deployer or supervisor owns:

- creating one private socket directory and mount per principal;
- making only the intended workload a member of that filesystem capability;
- removing the workload's other network paths;
- choosing the principal and invocation identifier in trusted broker
  configuration;
- supervising the two containers and deciding whether their failure is fatal
  to the larger execution.

The application owns:

- opening and retaining the returned client handle;
- using its channel end with `maelys_egress_client_connect`;
- closing the handle when the execution is over.

The socket is a capability. Possession of its private mount authorizes a
channel for the one principal configured on the broker; the request carries no
principal, invocation identifier, token or destination. A socket is never
shared by applications that should receive different identities. Peer
credentials may be checked as defence in depth, but are not the portable
identity contract across container user namespaces.

The bootstrap does not make Egress a sandbox. A workload with another network
path can ignore both it and the channel.

## Two protocols, not one enlarged protocol

The bootstrap connection is an `AF_UNIX` `SOCK_STREAM`. Its one successful
response carries the client end of an anonymous `AF_UNIX` `SOCK_DGRAM` pair.
After that, all destination requests and returned relay streams speak the
unchanged channel v1 contract on the datagram descriptor.

```text
bootstrap SOCK_STREAM by pathname
    OPEN(channel version 1)
    OK + one channel descriptor
    connection remains open as the lease

anonymous channel SOCK_DGRAM
    MECQ(host, port)
    MECP(status) + one relayed stream on OK
```

The separation is intentional. Path discovery, capability authorization and
lifetime belong to the bootstrap. Destinations, SNI, quotas and receipts stay
in the frozen channel.

## Transport and framing

The listener is filesystem `AF_UNIX` `SOCK_STREAM`. `SOCK_SEQPACKET` is not a
portable alternative: Linux provides it for Unix sockets and macOS does not.
The original maelys-system `fdpass` operations accept Unix `SOCK_DGRAM`
sockets only. System 0.11.0 now supplies the two additive partial stream I/O
operations, `maelys_sys_fd_stream_send` and
`maelys_sys_fd_stream_receive`, in the same standalone `fdpass.o`. They move
bytes and descriptors, not frames: every delivered descriptor is observed,
surplus descriptors are closed, and returned descriptors are `CLOEXEC`.
Egress owns fixed frame sizes, progress, descriptor cardinality, abandonment
and absolute deadlines. Extending the old datagram functions by relaxing
their socket-type check is not sufficient: their send has no byte count.

There is one request and one response per accepted connection. Both have fixed
sizes, so the receiver reads exactly that size across short reads. Every read
of an untrusted stream is a `recvmsg` with room for every descriptor either
supported kernel can attach; using `read` for an initial fragment would repeat
the macOS descriptor-leak class corrected in Egress 0.22.1.
Neither `read`, `recv` nor `MSG_PEEK` is permitted on the bootstrap or lease.
Even zero bytes received can carry rights on macOS; discarded surplus must
remain observable, not become a false EOF. Linux `SO_PASSCRED` can accompany
EOF with credentials alone, which must not keep a dead lease alive.

The response descriptor is attached to the first response byte. A short
`sendmsg` may deliver the descriptor with only part of the fixed frame; the
sender writes the remaining bytes without attaching it again. A receiver does
not accept the descriptor until the entire response has arrived and validated.
If the frame ends early, every descriptor already received is closed.
An unsuccessful send with zero progress retains the descriptor for a retry;
positive byte progress means the descriptor was queued once, not that the
peer acknowledged it. Empty rights-bearing sends are refused by the shared
stream API, since Linux and macOS give them different meanings.

The accepted socket and the listener are nonblocking. Finite absolute
monotonic deadlines bound connect, request receipt and response delivery;
signals consume the same interval rather than restarting it.
The shared stream operations must verify nonblocking mode without changing
the socket's flags. A macOS blocking send with rights can wait when only part
of the needed buffer space is free; the observed full-queue `EMSGSIZE` is not
a guarantee of nonblocking execution. Egress bounds work per reactor turn
as well as total elapsed time, including control-only records.

## Messages

All integers are big-endian. Offsets are in bytes.

### Open request — client to broker, 8 bytes

| offset | size | field | value |
|---|---|---|---|
| 0 | 4 | magic | `0x4d454251` (`MEBQ`) |
| 4 | 1 | bootstrap_version | `1` |
| 5 | 1 | channel_version | `1` |
| 6 | 2 | reserved | `0` |

The request carries no ancillary data. The broker closes every descriptor
attached to any request fragment before it does anything else and answers
`MALFORMED`. A wrong magic or non-zero reserved field is `MALFORMED`. An
unknown bootstrap or requested channel version is `UNSUPPORTED`; the response
still carries the broker's bootstrap version and the channel version it can
provide.

The client writes nothing after this request. A further byte at any time is a
protocol violation: before delivery the broker may answer `MALFORMED`; after
delivery it closes the lease and destroys the channel. No request identifier
or principal field is reserved for a later interpretation.

### Open response — broker to client, 16 bytes

| offset | size | field | value |
|---|---|---|---|
| 0 | 4 | magic | `0x4d454250` (`MEBP`) |
| 4 | 1 | bootstrap_version | `1` |
| 5 | 1 | status | one code of the table below |
| 6 | 1 | channel_version | `1` when supported; otherwise the broker's version |
| 7 | 1 | reserved | `0` |
| 8 | 8 | connect_timeout_ms | finite and non-zero on `OK`; `0` otherwise |

Ancillary data: exactly one descriptor on `OK`, none on every other status.
The descriptor is the client end returned by `maelys_egress_channel_create`,
not a connected destination stream. A client rejects another response size,
magic, bootstrap version, reserved value, descriptor cardinality, ancillary
type or truncated control data. It closes every descriptor received with a
malformed response.

`connect_timeout_ms` is the broker's per-destination deadline for the channel
it just created. Carrying it here closes the out-of-band gap deliberately left
to an inheriting supervisor by channel v1: the remote application can choose a
read deadline that is not shorter than the server's.

### Status codes

| code | name | when |
|---|---|---|
| 0 | `OK` | one channel descriptor is attached and the lease begins |
| 1 | `DENIED` | an enabled peer check refuses the accepted process |
| 2 | `BUSY` | the configured pending or active channel bound is full |
| 3 | `CANCELLED` | the broker is stopping |
| 4 | `MALFORMED` | the request violates this document |
| 5 | `UNSUPPORTED` | the bootstrap or requested channel version is unknown |
| 6 | `RESOURCE` | the broker could not allocate or create the channel |
| 7 | `INTERNAL` | anything else; diagnostics stay on the broker side |

These are bootstrap codes, not `errno`, not the channel status table and not a
numeric mirror of a C result enumeration. A client treats an unknown code as
`INTERNAL`. A local failure before a response — path refusal, connection
failure, EOF or client deadline — remains a local client result.

The numeric spaces are deliberately local to their frames. In particular,
code `2` is bootstrap `BUSY` but channel-v1 `TIMEOUT`. A client first selects
the protocol from the frame magic (`MEBP` or `MECP`) and only then interprets
the status; it must not feed both values through one common numeric enum.

## The lease

The accepted stream becomes a lifetime lease after an `OK` response. Neither
side sends another byte. The broker retains all three of:

- the accepted lease descriptor;
- the `maelys_egress_channel_t` server handle;
- one active-channel capacity slot.

It closes the copy of the client channel descriptor immediately after the
response send succeeds or fails. On success, EOF or any byte on the lease
causes the broker reactor to close the lease, request channel stop and enqueue
its destruction. A separate, fixed cleanup worker joins the channel thread;
only completed destruction releases the slot. Any
rights or unexpected ancillary data on the lease are also a protocol
violation: the worker receives them through the shared control-aware stream
operation, closes them and retires the channel in the same way. Polling for
readability followed by an ordinary byte read is not safe. On failed
or incomplete response delivery, the worker closes the accepted connection
and retires the channel without waiting for its thread. A process exit closes
both client descriptors and therefore reclaims the server-side thread even on a kernel
where closing the peer of a datagram pair is not observable while idle.

The worker is not the Egress server owner thread. Channel destruction waits
for the channel thread, and that thread may be waiting for a connector command
which only the server owner reactor can complete. Destruction on the owner
reactor would therefore stop all proxy and connector progress until the open
deadline. Joining on the broker reactor would instead stop new handshakes,
lease processing and enforcement of their deadlines. Neither reactor may
join a channel thread: the cleanup worker waits without holding the queue
lock, while both reactors continue to progress. The queue is bounded by the
existing capacity slots; there is no allocation or new thread per retirement.
These separations and the capacity accounting are required implementation
invariants, not optimizations.

The lease is necessary. Channel v1 records that idle datagram-pair peer closure
is not reported consistently by Linux and macOS; a broker that passed the
datagram descriptor and forgot the accepted stream could retain one thread and
connector reference forever for every exited idle client.

Closing the lease destroys the channel but does not revoke relay streams that
were already handed over, preserving channel v1. Stopping the Egress server
closes all leases, destroys all channels and revokes active streams through the
existing server-stop semantics.

Destroying the independently embedded broker alone closes its leases and
channels, but does not stop the shared server or revoke returned streams.
Only stopping that server revokes them. The library reports socket cleanup
failures rather than removing a replacement inode. Its worker notices server
stop every 50 ms independently of channel joins. It closes all leases, requests
all channel stops and removes the listener before any pending cleanup finishes.
Destroying the broker handle joins both workers and may wait for pending opens;
it must not run on the server owner reactor. Cleanup does not delay remaining
handshakes or leases while the broker is running.

A conforming client keeps the lease and channel descriptor in one opaque
handle and closes them together. V1 deliberately offers no `take_fd` operation
that would orphan the lease. A future launcher that places the channel on file
descriptor 4 must also preserve the lease on another descriptor for the full
child lifetime.

## Identity and filesystem authorization

The broker is configured with one canonical principal and optional invocation
identifier before it opens the listener. It creates every channel from that
identity. The wire contains no way to select or replace it.

The listener path is absolute, canonical and absent at startup. Its immediate
parent is held open and checked without following a symlink. V1 supports two
deployment forms:

- parent owned by the broker's effective UID, mode `0700`; the client runs as
  that UID and the socket is mode `0600`;
- parent owned by the broker's effective UID, mode `2750`, with a capability
  group to which the broker belongs; the socket inherits or is assigned that
  group and is mode `0660`. The application receives only that supplementary
  group and mount.

The parent is never writable by the client group. The broker unlinks only the
socket inode it created, after comparing its held identity, and removes it on
normal shutdown. A stale or replaced path is a startup or shutdown failure,
not something silently overwritten.

Filesystem permissions are the authorization boundary. A peer-credential
check may narrow them, but it cannot broaden them and its numeric UID or GID is
not recorded as the Egress principal. Container user-namespace mappings make
that numeric value a deployment fact rather than a portable identity.

## Bounds and failure behaviour

The broker has one finite combined bound for pending handshakes, active leases
and channels queued for or undergoing destruction. A connection occupies a
slot from `accept()` until its handshake fails without a channel, or its channel
has been fully destroyed after lease closure or delivery failure. It creates
no channel until a complete valid request is
present and a slot has been reserved. A client that trickles bytes, never reads
its response, or holds an idle lease consumes at most one bounded slot until
its deadline or closure; if it has a channel, the slot remains charged until
cleanup completes. Saturation including retiring channels returns `BUSY`, not
an unbounded cleanup backlog. Completed cleanup is collected on acceptance and
at least every 50 ms in the running reactor.

The handshake deadline covers accept-to-complete-response as one monotonic
interval. The client has its own finite open deadline covering socket creation,
connect, request write and complete response receipt. Neither deadline is sent
by the requester or extended by progress.

Failure paths close, in order where applicable:

1. every descriptor received unexpectedly;
2. the broker's copy of the client channel end;
3. the accepted lease, then request channel stop and enqueue destruction;
4. the channel server handle, on the cleanup worker;
5. the reserved capacity slot, only after destruction completes.

Diagnostics and lifecycle events never contain a credential. The bootstrap has
none to disclose.

## Proposed client library surface

`libmaelys_egress_client` remains the only code required in the confined
application. It gains an opaque handle rather than returning a bare descriptor:

```c
typedef struct maelys_egress_client_channel maelys_egress_client_channel_t;

maelys_egress_client_result_t maelys_egress_client_channel_open(
    const char *absolute_path,
    uint64_t open_timeout_ms,
    maelys_egress_client_channel_t **out_channel,
    char **out_error);

int maelys_egress_client_channel_fd(
    const maelys_egress_client_channel_t *channel);

uint64_t maelys_egress_client_channel_connect_timeout_ms(
    const maelys_egress_client_channel_t *channel);

void maelys_egress_client_channel_close(
    maelys_egress_client_channel_t *channel);
```

The names and signatures are part of this proposal, not yet ABI. `open` uses
one finite non-zero absolute deadline and returns only after the response is
complete, valid, and the received channel descriptor is `CLOEXEC`. The handle
owns both descriptors. Its borrowed channel descriptor is passed to the
existing `maelys_egress_client_connect`; the caller uses a read timeout at
least as large as the reported broker connect timeout.

This additive opaque type and these new functions leave
`MAELYS_EGRESS_CLIENT_ABI_VERSION` at 1. The repository changes that number
when an existing layout or semantic contract becomes incompatible, not when a
header only gains symbols. The standalone boundary does not change: the
archive contains no Egress core and no thread runtime. It may take from
maelys-system only the existing member `fdpass.o`, extended with the stream
operations, with no undefined `maelys_sys_` or `pthread_` symbol. No second
transport object, thread runtime, clock or frame-state type is added to System.

## Proposed CLI and configuration surface

The command is:

```text
maelys-egress channel broker --config FILE
```

Its stable identifier is `channel.broker`. It is a long-running `stream`
command whose `protocol-stream` stdout remains
`maelys-egress-lifecycle/1`; rendering flags are therefore rejected. Exit 0 is
a graceful stop, exit 1 a startup or execution failure. Exit 2 is not used.
Before `ready`, failures are framework envelopes on stderr. After `ready`,
receipts, reload events and shutdown events have the same meanings as `serve`.

The lifecycle schema gains an optional `channel` member on `ready`:

```json
{
  "transport": "unix",
  "path": "/run/maelys-egress/agent-001.sock",
  "protocol": "maelys-egress-channel-bootstrap/1"
}
```

`channel broker` emits `channel` and no `proxy`; `serve` output is unchanged.
The configured path in the ready event contains no secret.

As required by the CLI contract, the command has no operational listener,
identity, deadline or limit option. Proposed configuration keys are:

| key | required/default | meaning |
|---|---|---|
| `channel_listen_unix` | required in broker mode | private bootstrap pathname |
| `channel_principal` | required in broker mode | principal bound to every channel |
| `channel_invocation_id` | absent | optional receipt invocation identifier |
| `channel_connect_timeout_ms` | `5000` | per-destination channel deadline, `1..600000` |
| `channel_handshake_timeout_ms` | `5000` | complete bootstrap deadline, `1..60000` |
| `channel_max_clients` | `128` | pending handshakes, active leases and retiring channels, `1..4096` |

The configuration grammar stays at schema version 1: these are additive keys.
`channel_listen_unix` and `channel_principal` require each other. The other
channel keys require both. Broker mode conflicts with `listen`, `listen_unix`,
`unix_peer`, `token_file`, `unauthenticated_loopback` and every TLS listener key,
including explicitly supplied false/default values; the socket capability
replaces proxy credentials rather than hiding one in the confined process.
The existing destination policy, stream `max_connections`, audit settings and
admin listener remain available. Principal quota keys require either
`token_file` in proxy mode or `channel_principal` in broker mode.

`config validate` validates either complete mode from the keys it contains.
`serve` refuses a broker-mode configuration before `ready`, and
`channel broker` refuses a proxy-mode configuration before `ready`. No alias
or second spelling is introduced.

The core library must expose the same capability, not leave it in the CLI. The
implementation adds an independently embeddable opaque broker handle to
`libmaelys_egress`. It is created from one connector already bound to the
configured principal, retains that connector, and owns a reactor which
accepts bootstrap clients and watches leases, plus a fixed cleanup worker
which calls `maelys_egress_channel_destroy`. The listener and lease watches are
never registered on the server owner reactor. The CLI sets native-only mode,
then binds the configured identity with `maelys_egress_config_set_native_principal`.
`maelys_egress_server_native_connector_create` obtains that sole immutable
identity without manufacturing a bearer secret. It refuses every other server
mode; the ordinary credential API cannot authenticate a native bound principal.
`maelys_egress_channel_broker_is_running` lets the CLI stop with a fatal event
if its broker reactor fails. Readiness is emitted before any receipt or reload
event; `ready.channel` and `ready.proxy` are mutually exclusive.

These additive functions and opaque types leave `MAELYS_EGRESS_ABI_VERSION`
at 3. A bump is required only if implementation changes an existing public
layout or semantic contract; if that becomes necessary, this proposal must be
amended before the code relies on it.

## Compose result

The conformance example has two services and a private named volume:

```yaml
services:
  egress:
    command: ["channel", "broker", "--config", "/etc/maelys-egress.conf"]
    volumes:
      - channel:/run/maelys-egress

  app:
    network_mode: none
    volumes:
      - channel:/run/maelys-egress:ro

volumes:
  channel:
```

The real example will include initialization of the owner/group/modes, trusted
configuration, readiness, read-only roots, dropped capabilities and a finite
client retry. The application image links `libmaelys_egress_client`, opens the
bootstrap path, proves a direct network attempt fails, then reaches one allowed
destination through the returned native channel. It receives no token and does
not contain the Egress core or CLI.

The [native Compose implementation](../examples/compose-channel/README.md)
adds an initializer and a local two-port fixture to those two workloads.
Its `compose-channel-check` gate runs in the required Docker CI job and checks
actual container isolation, same-target direct failure, policy refusal and
receipts. This integration does not by itself freeze the wire tables: the
complete Linux/macOS conformance gate below still needs its final review.

## Conformance and adversarial gate

Implementation is incomplete until all of these are automated on Linux and
macOS. The checkboxes record executable coverage, not acceptance or a wire
freeze. Before freezing, review the results of the required CI checks on the
exact candidate commit, including the focused macOS gate and Compose.

Audit baseline: at `ec4a309` (PR #148), eight criteria were covered locally,
one relied on the pinned System's cross-kernel evidence, and eight were
partial. In particular, resetting the handshake deadline after each fragment
survived `make check`. A separate 120 ms / 75 ms trickle probe refused the
correct implementation after two bytes (160 ms observed), but the mutant
accepted all eight bytes after 544 ms. This demonstrated a missing regression
test, not a defect in the unmodified implementation. An eight-byte request
bounds that particular extension; it does not alone imply an infinite lease.
The proof and its permanent replacement below must survive `make clean`.

- [x] byte vectors for the valid request and every response status; explicit
  dispatch of code `2` as bootstrap `BUSY` under `MEBP` and channel `TIMEOUT`
  under `MECP`; wrong magic, both unknown versions and every non-zero reserved
  field. **Evidence:** `codec_tests` and `exchange` in
  [`test_bootstrap_client.c`](../tests/test_bootstrap_client.c); literal
  response vectors in `tests/test_broker_shutdown.py`.
- [x] one-byte-at-a-time request and response delivery, including a descriptor on
  the first response fragment. **Evidence:** `gate_fragments` in
  [`test_bootstrap_gates.c`](../tests/test_bootstrap_gates.c) forces successful
  reads to one byte, injects `WOULD_BLOCK` and `EINTR`, and asserts exactly
  8/16 bytes and the corresponding call counts. Kernel write coalescing cannot
  turn this into a whole-frame test. Existing split-write tests remain.
- [x] zero, one and multiple descriptors on `OK`; a descriptor on every non-`OK`
  status; ancillary truncation; all unexpected descriptors closed.
  **Evidence:** `exchange` tests zero/one/fifty rights and every refusal;
  `gate_fragments` injects `CONTROL_TRUNCATED` into the broker and client
  state machines, both on the first descriptor byte and after client ownership
  transfer. Both truncation mutants must fail; descriptor counts return to baseline.
- [x] fifty descriptors attached to a valid request, with the process descriptor
  count unchanged after `MALFORMED` on macOS and Linux.
  **Evidence:** `test_bootstrap_broker` in
  [`test_operations.c`](../tests/test_operations.c), with attachment at each byte offset.
- [x] rights attached to every fragment and to an idle lease, including macOS
  zero-byte control-only records; a control-only flood cannot monopolise the
  worker or extend an absolute deadline. **Evidence:** `test_bootstrap_broker`
  covers request offsets and lease rights. `gate_fairness` continuously
  reconnects rejected control-bearing peers (zero-byte control-only on macOS),
  while a healthy negotiation completes and a stalled request expires on time.
  It also keeps a non-reading peer alive throughout; all descriptors settle.
- [x] partial sends attach the descriptor exactly once; a zero-progress retry
  still attaches it; test empty/full queues, `EINTR`, Linux `SO_PASSCRED` and
  macOS near-full queues, not just a completely full queue.
  **Evidence:** [`test_broker_faults.c`](../tests/test_broker_faults.c) forces
  `WOULD_BLOCK`, `EINTR`, then sixteen one-byte sends: eighteen calls, one
  descriptor transfer. Kernel behavior remains owned by pinned maelys-system
  v0.11.0 (`c7d13e5`): `tests/test_fdpass_stream.c` and
  `tests/test_fdpass_faults.c` cover full/near-full queues, partial delivery,
  credentials, truncation and OS errors. Its
  [cross-host CI evidence](https://github.com/maelys-dev/maelys-system/actions/runs/36627566454)
  is used rather than implementing or remeasuring descriptor passing here.
- [x] a client that stops mid-request, never reads the response, trickles until the
  deadline, writes after `OK`, or exits while its channel is idle.
  **Evidence:** `gate_trickle` waits for actual consumption of successive
  fragments; the original absolute deadline must close the peer while another
  handshake succeeds. `gate_fairness` covers a non-reading peer; a spawned
  process signals its live idle lease before `SIGKILL`, then `gate_idle` and
  a new admission prove recovery. Existing operations cover post-`OK` writes.
- [x] response delivery failure after channel creation, with no channel thread,
  connector reference, descriptor or capacity slot left behind.
  **Evidence:** `gate_delivery` observes successful creation before hard failure,
  hard failure after the first descriptor byte, and permanent `WOULD_BLOCK`
  after that byte until the absolute deadline. `gate_idle` checks successful
  joins, destructions, connector reference count and descriptor baseline;
  a subsequent successful open reclaims the sole slot.
- [x] active-client saturation answers `BUSY`; completing cleanup after a lease
  closes admits the next. **Evidence:** `test_bootstrap_broker`, bounded
  retiring-slot and early-`EPIPE` cases in `test_broker_faults.c`, and the
  Compose example's one-slot reacquisition.
- [x] lease closure during a channel request and with an already returned stream,
  preserving the frozen channel shutdown distinction. **Evidence:**
  `test_broker_faults.c`, `test_bootstrap_broker` and the Compose consumer.
- [x] lease closure while a connector open is deliberately held pending, while a
  simultaneous proxy request still progresses on the server owner reactor,
  another bootstrap client opens, a partial handshake expires, and other
  leases close before the held open is released; retiring channels still count
  towards saturation, all capacity returns after cleanup, and descriptors,
  channel threads and connector references are released.
  **Evidence:** the deliberately suspended connector open in
  `test_broker_faults.c`; `bootstrap-blocking-destruction` must fail on its
  progress assertion, not a build failure or suite timeout.
- [x] broker destruction closes every lease and channel while returned streams
  survive; server stop also revokes those streams. **Evidence:**
  `test_bootstrap_broker`, `test_broker_faults.c` and
  `tests/test_broker_shutdown.py` (required CLI suite).
- [x] path symlink, pre-existing socket, wrong owner, wrong mode, writable
  capability directory and replaced-inode refusals. **Evidence:**
  `test_bootstrap_broker` covers existing socket, leaf symlink, both valid
  permission modes, writable group and replaced inode. `test_bootstrap_gates.c`
  adds immediate-parent and ancestor symlinks, and a test-only expected-UID
  substitution on an otherwise valid owned `0700` directory; no privilege
  or accidental mode failure stands in for the owner check.
- [x] client open deadline interrupted repeatedly without extending.
  **Evidence:** `exchange` attack 7 in `test_bootstrap_client.c`, repeated
  `SIGALRM` during an 80 ms deadline, with a 180 ms upper bound.
- [x] fuzzing both fixed-frame decoders and every split point of the stream frame.
  **Evidence:** [`fuzz_bootstrap.c`](../tests/fuzz/fuzz_bootstrap.c) decodes every
  prefix and checks successful round trips. `gate_fragments` covers actual
  reassembly at every byte boundary. Reviewed `MEBQ`/`MEBP` seeds in
  [`corpus/bootstrap`](../tests/fuzz/corpus/bootstrap) are stored as hex and
  materialized as exact 8/16-byte binary frames (not ASCII hex) for both replay
  and the 10,000-run ASan/UBSan fuzz gate.
- [x] a mutation gate holding identity absence, descriptor cardinality, capacity
  release, the lease-to-channel destruction edge and broker progress during
  pending channel destruction, plus absolute deadlines and request/response
  control truncation. **Evidence:** `scripts/mutation-check.sh` requires a
  test assertion, not compilation failure, for each bootstrap mutant. Linux
  runs the full gate; the already-required `native (macos-15, clang, clang++)`
  job runs only `bootstrap-mutation-check` and `bootstrap-fuzz-check` in
  addition to ordinary `make check`. No new optional job replaces protection.
- [x] the Compose example as a CI job, including exact network-mode assertions.
  **Evidence:** the required `docker` job runs
  [`test-compose-channel.py`](../scripts/test-compose-channel.py), inspecting
  actual network mode, mounts, UID/GID, mediation, refusal and surviving streams.

The existing channel vectors, fuzz target and adversarial operations tests
continue unchanged. Bootstrap conformance is additional, not a replacement.

## Implementation order

1. Fix System's existing byte-only Unix receive independently and rebuild
   Egress against that patch; static consumers do not inherit a fix merely
   because an installed System package was upgraded.
2. Amend System's admission rule explicitly for a transport variant of an
   already admitted primitive carrying measured kernel divergences. Egress
   is its one direct consumer; Warden's descriptor inheritance is not a
   second bootstrap consumer. Agree the two partial operations and add their
   descriptor-flood, partial-I/O, close-on-exec and fault tests in System.
3. Add the bootstrap codec and client opaque handle in
   `libmaelys_egress_client`.
4. Add the independent broker handle, reactor and bounded cleanup worker to
   the core library, including lease, destruction and capacity ownership;
   prove that neither reactor joins channels and that retiring channels
   retain their capacity until destruction finishes.
5. Declare `channel.broker` and the configuration catalog, handler, lifecycle
   schema, exact `describe` tests and generated references.
6. Add the Egress-aware Compose example and make its integration test required
   in CI.
7. Freeze the implemented tables under `protocol/` only when every gate above
   passes.

`channel exec` is independent and may follow: it creates an anonymous channel
and inherits it directly, so it needs no bootstrap. A future FD-4 launcher over
this broker must preserve the lease as well as the channel descriptor.

## Review decisions before implementation

1. System owns two partial, control-aware Unix-stream operations in
   `fdpass.o`, with explicit byte counts and descriptor ownership. Egress
   owns framing, progress and deadlines. There is no public frame-state
   structure and no exception to Egress's `sendmsg`/`recvmsg` boundary audit.
   Both kernels' measured behavior and the receive output flags must support
   descriptor rejection even with zero byte progress before the API freezes.
2. Both `0700` same-UID and `2750` capability-group parents are supported and
   tested. Their invariant is identical: the broker owns the parent and the
   client group can never modify it.
3. The core surface is an independently embeddable broker handle with its own
   reactor and bounded cleanup worker, created from a connector. Neither the
   broker reactor nor the server owner reactor joins channel threads. It is
   not configuration or watches grafted onto `maelys_egress_server_t`. Exact additive function names remain
   an implementation review; the execution boundary does not.
