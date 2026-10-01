# Maelys Egress channel bootstrap, version 1

**Status: frozen contract.** Protocol identifier:
`maelys-egress-channel-bootstrap/1`. First published in Egress 0.23.0.
The wire format and ownership rules below are normative. The original
[mediated-connection channel v1](egress-channel-v1.md) is unchanged.

## Boundary and transport

A separately started, Egress-aware process connects to a filesystem
`AF_UNIX` `SOCK_STREAM` socket. One request and one response deliver the
client end of an anonymous `AF_UNIX` `SOCK_DGRAM` channel with `SCM_RIGHTS`.
The bootstrap stream then remains open as the channel's lifetime lease.
Destination requests and returned relay streams use channel v1 on the
datagram descriptor, not on the bootstrap stream.

The socket is a capability for one principal and optional invocation ID,
fixed by the trusted broker configuration or connector. No request names an
identity, token or destination. Use one private socket directory and mount
per principal; do not share it with applications requiring another identity.
Numeric peer UIDs are not the portable principal contract across containers.

The supervisor owns socket access, process creation and removal of other
network paths. Egress is not a sandbox. Bootstrap neither assigns descriptor
number 4 nor injects a descriptor into an already running process. An
application using this interface must understand Egress's native channel.
An inheriting supervisor needs no bootstrap at all.

## Framing and descriptor transfer

There is one 8-byte request and one 16-byte response per connection. Integers
are unsigned and big-endian; offsets and lengths are in bytes. Receivers
accumulate short reads without reading past the remaining frame length.
There is no acknowledgement, multiplexing or subsequent bootstrap request.

Every bootstrap and lease read must collect ancillary data, even when no
descriptor is expected. Never use `read`, `recv` or `MSG_PEEK` on these
streams. Egress uses the control-aware partial stream operations in
maelys-system's `fdpass.o`; framing, progress and deadlines belong to Egress.
Surplus descriptors must be closed and remain observable to the protocol.
Unexpected ancillary types or truncated control data invalidate a frame.

An `OK` response attaches exactly one descriptor to its **first byte**.
The receiver initially reads exactly one byte to verify this position, then
rejects any further descriptors while collecting the remaining bytes. Every
descriptor received on an abandoned or malformed frame must be closed.
The returned descriptor must be a connected Unix datagram socket and have
`FD_CLOEXEC` set before it is exposed to the caller. On macOS this flag is
set after receipt, not atomically against concurrent `fork` plus `exec`;
embedders must coordinate such execution when that race matters.

A positive partial send queues the descriptor once. The sender closes its
local copy and sends remaining bytes without attaching it again. A retry
after zero progress retains that copy. Any failed or incomplete delivery
closes remaining local copies and retires the channel. Success means bytes
and rights were queued, not that the application acknowledged receipt.
Empty rights-bearing sends are forbidden.

macOS can deliver zero bytes with rights: this is malformed control, not
EOF or a harmless retry. Linux can deliver credentials with EOF when
`SO_PASSCRED` is enabled: it must not keep a dead lease alive. A clean EOF,
reset or unrecoverable I/O error terminates the exchange. These distinctions
are provided by the shared stream operations, not a second raw-syscall
implementation in Egress.

## Open request

| offset | size | field | value |
|---|---|---|---|
| 0 | 4 | magic | `0x4d454251` (`MEBQ`) |
| 4 | 1 | bootstrap_version | `1` |
| 5 | 1 | channel_version | `1` |
| 6 | 2 | reserved | `0` |

The request carries no ancillary data. The broker closes any received
descriptors and refuses unexpected control with `MALFORMED`. Wrong magic or
nonzero reserved bytes are also `MALFORMED`. With otherwise valid framing,
an unknown bootstrap or channel version yields `UNSUPPORTED`, carrying the
broker's supported versions. Errors are best-effort replies: a broken
transport or expired deadline can instead close the connection.

The client writes nothing after the request. Extra bytes or control are a
protocol violation: before delivery the broker may reply `MALFORMED` or
close; after delivery it closes the lease and retires the channel.

## Open response

| offset | size | field | value |
|---|---|---|---|
| 0 | 4 | magic | `0x4d454250` (`MEBP`) |
| 4 | 1 | bootstrap_version | `1` |
| 5 | 1 | status | bootstrap code below |
| 6 | 1 | channel_version | `1`, except the supported version on `UNSUPPORTED` |
| 7 | 1 | reserved | `0` |
| 8 | 8 | connect_timeout_ms | nonzero on `OK`, zero otherwise |

`OK` carries exactly one channel descriptor; every refusal carries none.
The client rejects incorrect framing, versions, reserved bytes, timeout
presence or descriptor cardinality, and closes all received descriptors.
An unknown channel version is accepted only in an `UNSUPPORTED` refusal.
An unknown bootstrap response version is not interpreted as v1.

The timeout is the server's per-destination connect deadline in milliseconds,
not the bootstrap deadline and not a user-selected value. On the wire it is
a nonzero unsigned 64-bit integer, with no infinity sentinel. Egress's broker
API accepts `1..600000` ms. Choose a channel read timeout at least as large;
the client can read it from the returned handle.

| code | name | meaning | client result suffix |
|---|---|---|---|
| 0 | `OK` | channel delivered, lease established | `OK` |
| 1 | `DENIED` | capability authorization refused | `ERR_DENIED` |
| 2 | `BUSY` | combined capacity bound exhausted | `ERR_BUSY` |
| 3 | `CANCELLED` | broker stopping | `ERR_CANCELLED` |
| 4 | `MALFORMED` | request violates this contract | `ERR_PROTOCOL` |
| 5 | `UNSUPPORTED` | requested version unsupported | `ERR_UNSUPPORTED` |
| 6 | `RESOURCE` | allocation/resource failure | `ERR_RESOURCE` |
| 7 | `INTERNAL` | other broker failure | `ERR_INTERNAL` |

These are protocol codes, never platform `errno` or numeric C-enum aliases.
The result prefix is `MAELYS_EGRESS_CLIENT_`. An unknown status with otherwise
valid refusal framing maps to `ERR_INTERNAL`. Local argument, I/O and open
deadline failures are local client results, not fabricated wire statuses.

The current broker authorizes through filesystem permissions; it has no
optional peer-check setting emitting `DENIED`. Shutdown may simply close
the socket, without sending `CANCELLED`. Clients must understand both codes
but must not require either reply. Channel-creation memory failure maps to
`RESOURCE`; other creation failures map to `INTERNAL`.

At capacity the broker makes one best-effort `BUSY` send and closes, without
allocating a slot or channel or waiting for a request. Clients may receive
that refusal even if their request write failed. They must not accept `OK`
after an incomplete request.

The status namespace belongs to the magic: `MEBP` code 2 is **BUSY**;
channel-v1 `MECP` code 2 is **TIMEOUT**. Decode the magic before the status.

### Byte vectors

Spaces below only separate fields; ancillary descriptors are not bytes in
these strings.

```text
request:         4d454251 01 01 0000
OK, 5000 ms:     4d454250 01 00 01 00 0000000000001388  (+ one channel FD)
BUSY:            4d454250 01 02 01 00 0000000000000000  (no FD)
MALFORMED:       4d454250 01 04 01 00 0000000000000000  (no FD)
UNSUPPORTED:     4d454250 01 05 01 00 0000000000000000  (no FD)
```

## Lease, ownership and shutdown

After `OK`, neither endpoint sends any more bootstrap data. The broker owns
the accepted lease, channel server handle and capacity slot. EOF, extra
bytes or ancillary data close the lease and request channel stop. Destruction
runs on a fixed cleanup worker, never on the broker or Egress server reactor.
The bounded retirement queue uses existing slots, with no new allocation or
thread per retirement. A slot is reusable only after its channel is joined.

A pending destination open may delay a join, but must not delay either
reactor's handshakes, lease handling or deadlines. Completed cleanup and
server stop are observed by the running broker at least every 50 ms,
subject to host scheduling. Synchronous broker destruction joins its workers
and may wait for pending opens; it must not run on the server owner thread.

Lease closure destroys the channel but **does not revoke destination streams
already returned**. Destroying the broker alone has the same property.
Stopping the Egress server revokes streams, closes leases and stops channels;
the caller still destroys its broker handle. Process death closes a client's
lease even on hosts that cannot detect idle datagram-pair peer closure.

The standalone client's opaque handle owns both the lease and channel.
`maelys_egress_client_channel_open` returns only after validating the full
response. `maelys_egress_client_channel_fd` returns a borrowed descriptor;
use it with `maelys_egress_client_connect`, never close it separately.
`maelys_egress_client_channel_connect_timeout_ms` reports the broker deadline.
`maelys_egress_client_channel_close` closes both descriptors. Externally
serialize operations on one handle, including close. There is no `take_fd`
operation: a future launcher inheriting the channel must preserve its lease
as well, for the complete child lifetime.

The client archive needs neither Egress core nor a thread runtime. It includes
only System's standalone `fdpass.o`, with no unresolved `maelys_sys_` or
`pthread_` symbol. These opaque APIs were added without breaking core
revision 3 or client revision 1; the public headers specify their C
signatures and number the interface (core revision 4, client revision 2,
since the release that adopted two ABI numbers).

## Filesystem capability and bounds

The listener pathname is absolute, canonical and absent at startup. Its
parent and ancestors must not be symbolic links. The broker holds and checks
the immediate parent, owned by its effective UID, with exactly either:

- `0700`, for a same-UID client and a `0600` socket; or
- `2750`, with the broker a member of the capability group and a `0660`
  socket in that group. The client group cannot modify the parent.

The deployer must keep that namespace trusted throughout use. Cleanup checks
the created socket's identity and reports missing/replaced paths instead of
silently removing another inode. Startup does not overwrite a stale socket.

The listener, accepted sockets and client bootstrap socket are nonblocking.
One absolute monotonic broker deadline covers acceptance through complete
response delivery. One finite nonzero client deadline covers socket creation,
connect, request and complete response. Neither progress nor interruptions
restart either interval; work per reactor turn is also bounded.

Pending handshakes, active leases and retiring channels share one capacity
bound. Channels are created only after a complete valid request. A non-reader
whose full response has reached the kernel is an idle lease, not an unfinished
handshake; it remains charged until closure. V1 defines no lease idle timeout
and no read acknowledgement. Bounds limit resource use, not availability
against authorized peers deliberately holding every slot.

## CLI and integration

```sh
maelys-egress channel broker --config /etc/maelys-egress.conf
```

The command's identifier is `channel.broker`; stdout is the
`maelys-egress-lifecycle/1` protocol stream, not a rendered envelope. Rendering
flags are rejected. Graceful stop exits 0, failure exits 1; it does not use
exit 2. Before readiness, failures are framework envelopes on stderr. Broker
readiness has `channel` and no `proxy`; its protocol member names this
bootstrap contract. Receipts, reload and stop events follow the shared
[lifecycle schema](egress-lifecycle-v1.schema.json).

Operational settings stay in the configuration, not command-line options:

| key | default or requirement | bound |
|---|---|---|
| `channel_listen_unix` | required | private canonical pathname |
| `channel_principal` | required | one immutable principal |
| `channel_invocation_id` | absent | optional invocation correlation |
| `channel_connect_timeout_ms` | `5000` | `1..600000` |
| `channel_handshake_timeout_ms` | `5000` | `1..60000` |
| `channel_max_clients` | `128` | `1..4096`, including retirement |

The configuration remains schema version 1. Broker mode excludes proxy
listener, credential, unauthenticated-loopback and TLS-listener settings,
even explicitly supplied default values. Policy, quotas, audit and the admin
listener remain available. `serve` refuses broker configuration and
`channel broker` refuses proxy configuration. Consult the installed generated
CLI and configuration references for their full declarations.

The native Compose example installs an Egress-aware application in a separate
`network_mode: none` container using a read-only capability mount and no
proxy secret. The application links only the client archive. Compose proves
one deployment; it does not make arbitrary mounts or network setups safe.

## Conformance and freeze evidence

The 17-criterion checklist and design history remain in the repository's
`proposals/egress-channel-bootstrap-v1.md`. The pre-freeze review of PR #149,
head `160b125`, verified required Linux and macOS gates: 23 Linux mutations,
9 focused macOS mutations and 10,000 bootstrap fuzz executions per host,
plus the required native Compose gate. Tests exercise literal vectors,
deterministic fragmentation, truncated control, descriptor ownership,
absolute deadlines, cleanup, saturation, lease death and reactor progress.
System 0.11.0 supplies the measured cross-kernel descriptor-passing behavior;
Egress does not reimplement it. These gates remain required after the freeze.

Changes to the byte format or established wire meaning require a new
protocol version. This freeze adds no bytes, statuses or runtime behavior
to the implementation reviewed by those gates.
