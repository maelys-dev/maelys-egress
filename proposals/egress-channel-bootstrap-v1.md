# Maelys Egress channel bootstrap, v1

**Status: proposal — not implemented. Nothing here is a contract.** The
mediated-connection channel itself remains the frozen
[`egress-channel-v1`](../protocol/egress-channel-v1.md) contract. This document
proposes only how a separately started process obtains one such channel from a
broker by pathname. It moves to `protocol/` only after the library, CLI,
portable descriptor-passing dependency, vectors and adversarial gate named
below exist. Until then this is the place to disagree.

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
- the broker listener, bound at creation to one execution principal;
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
The current maelys-system `fdpass` primitive accepts connected Unix
`SOCK_DGRAM` sockets only, so implementation depends on a maelys-system
primitive that passes descriptors with a fixed stream frame and retains the
same guarantees: every delivered descriptor is observed, surplus descriptors
are closed, and returned descriptors are `CLOEXEC`.

There is one request and one response per accepted connection. Both have fixed
sizes, so the receiver reads exactly that size across short reads. Every read
of an untrusted stream is a `recvmsg` with room for every descriptor either
supported kernel can attach; using `read` for an initial fragment would repeat
the macOS descriptor-leak class corrected in Egress 0.22.1.

The response descriptor is attached to the first response byte. A short
`sendmsg` may deliver the descriptor with only part of the fixed frame; the
sender writes the remaining bytes without attaching it again. A receiver does
not accept the descriptor until the entire response has arrived and validated.
If the frame ends early, every descriptor already received is closed.

The accepted socket and the listener are nonblocking. Finite absolute
monotonic deadlines bound connect, request receipt and response delivery;
signals consume the same interval rather than restarting it.

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

## The lease

The accepted stream becomes a lifetime lease after an `OK` response. Neither
side sends another byte. The broker retains all three of:

- the accepted lease descriptor;
- the `maelys_egress_channel_t` server handle;
- one active-channel capacity slot.

It closes the copy of the client channel descriptor immediately after the
response send succeeds or fails. On success, EOF or any byte on the lease
causes the broker to destroy the channel and release the slot. On failed or
incomplete response delivery, it destroys the channel before releasing the
accepted connection. A process exit closes both client descriptors and
therefore reclaims the server-side thread even on a kernel where closing the
peer of a datagram pair is not observable while idle.

The lease is necessary. Channel v1 records that idle datagram-pair peer closure
is not reported consistently by Linux and macOS; a broker that passed the
datagram descriptor and forgot the accepted stream could retain one thread and
connector reference forever for every exited idle client.

Closing the lease destroys the channel but does not revoke relay streams that
were already handed over, preserving channel v1. Stopping the Egress server
closes all leases, destroys all channels and revokes active streams through the
existing server-stop semantics.

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
parent is held open and checked without following a symlink. Two deployment
forms are proposed:

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

The broker has one finite combined bound for pending handshakes and active
leases. A connection occupies a slot from `accept()` until its lease closes or
the handshake fails. It creates no channel until a complete valid request is
present and a slot has been reserved. A client that trickles bytes, never reads
its response, or holds an idle lease consumes at most one bounded slot until
its deadline or closure.

The handshake deadline covers accept-to-complete-response as one monotonic
interval. The client has its own finite open deadline covering socket creation,
connect, request write and complete response receipt. Neither deadline is sent
by the requester or extended by progress.

Failure paths close, in order where applicable:

1. every descriptor received unexpectedly;
2. the broker's copy of the client channel end;
3. the channel server handle;
4. the accepted lease;
5. the reserved capacity slot.

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

Adding this surface bumps `MAELYS_EGRESS_CLIENT_ABI_VERSION` from 1 to 2. The
standalone boundary does not change: the archive contains no Egress core and
no thread runtime. It may take from maelys-system only the new standalone
stream descriptor-passing object beside the existing `fdpass.o`, with no
undefined `maelys_sys_` or `pthread_` symbol.

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
| `channel_max_clients` | `128` | pending plus active leases, `1..4096` |

The configuration grammar stays at schema version 1: these are additive keys.
`channel_listen_unix` and `channel_principal` require each other. The other
channel keys require both. Broker mode conflicts with `listen`, `listen_unix`,
`unix_peer`, `token_file` and `unauthenticated_loopback`; the socket capability
replaces proxy credentials rather than hiding one in the confined process.
The existing destination policy, stream `max_connections`, audit settings and
admin listener remain available. Principal quota keys require either
`token_file` in proxy mode or `channel_principal` in broker mode.

`config validate` validates either complete mode from the keys it contains.
`serve` refuses a broker-mode configuration before `ready`, and
`channel broker` refuses a proxy-mode configuration before `ready`. No alias
or second spelling is introduced.

The core library must expose the same capability, not leave it in the CLI. The
implementation is expected to add a channel-broker configuration surface to
`libmaelys_egress`, integrate the listener with the server owner thread and
bind its configured principal without manufacturing a bearer secret. Exact C
names are reviewed with the implementation; changing the public core header
will bump the Egress ABI from 3 to 4.

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

## Conformance and adversarial gate

Implementation is incomplete until all of these are automated on Linux and
macOS:

- byte vectors for the valid request and every response status; wrong magic,
  both unknown versions and every non-zero reserved field;
- one-byte-at-a-time request and response delivery, including a descriptor on
  the first response fragment;
- zero, one and multiple descriptors on `OK`; a descriptor on every non-`OK`
  status; ancillary truncation; all unexpected descriptors closed;
- fifty descriptors attached to a valid request, with the process descriptor
  count unchanged after `MALFORMED` on macOS and Linux;
- a client that stops mid-request, never reads the response, trickles until the
  deadline, writes after `OK`, or exits while its channel is idle;
- response delivery failure after channel creation, with no channel thread,
  connector reference, descriptor or capacity slot left behind;
- active-client saturation answers `BUSY`; closing one lease admits the next;
- lease closure during a channel request and with an already returned stream,
  preserving the frozen channel shutdown distinction;
- broker stop closes every lease and channel and revokes active streams;
- path symlink, pre-existing socket, wrong owner, wrong mode, writable
  capability directory and replaced-inode refusals;
- client open deadline interrupted repeatedly without extending;
- fuzzing both fixed-frame decoders and every split point of the stream frame;
- a mutation gate holding identity absence, descriptor cardinality, capacity
  release and the lease-to-channel destruction edge;
- the Compose example as a CI job, including exact network-mode assertions.

The existing channel vectors, fuzz target and adversarial operations tests
continue unchanged. Bootstrap conformance is additional, not a replacement.

## Implementation order

1. Agree this proposal with maelys-system on the fixed-frame Unix-stream
   descriptor primitive; measure Linux and macOS before freezing it.
2. Add that standalone primitive and its descriptor-flood, partial-frame,
   close-on-exec and fault tests in maelys-system.
3. Add the bootstrap codec and client opaque handle in
   `libmaelys_egress_client`.
4. Add the broker to the core library, including lease and capacity ownership.
5. Declare `channel.broker` and the configuration catalog, handler, lifecycle
   schema, exact `describe` tests and generated references.
6. Add the Egress-aware Compose example and make its integration test required
   in CI.
7. Freeze the implemented tables under `protocol/` only when every gate above
   passes.

`channel exec` is independent and may follow: it creates an anonymous channel
and inherits it directly, so it needs no bootstrap. A future FD-4 launcher over
this broker must preserve the lease as well as the channel descriptor.

## Review questions before implementation

1. Do maelys-system and Egress agree that one fixed stream frame, received
   control-aware across every short read, is the primitive to share rather
   than generalising datagram semantics onto `SOCK_STREAM`?
2. Are `0700` same-UID and `2750` capability-group parents the two supported
   deployment forms, or should v1 require the group form only?
3. Should the first public core surface be configuration on
   `maelys_egress_server_t`, as proposed, or an independently embeddable broker
   handle? The wire, client API and CLI contract do not depend on that choice.
