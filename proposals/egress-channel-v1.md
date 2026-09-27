# Maelys Egress mediated-connection channel, v1

**Status: proposal — not implemented.** Nothing here is a contract. It becomes
one when this repository holds the implementation, the conformance vectors and
the adversarial gate this document names; v1 freezes then, and the normative
byte tables move to `protocol/`, where installed contracts live. Until then this
file is the place to disagree.

## Why

`maelys_egress_connector_session_open` ([egress.h](../include/maelys/egress.h))
returns a policy-checked TCP stream to an embedder in the same process. A
sandboxed workload lives in another process; someone must carry its request
across that boundary and hand the stream back. Today that someone is
maelys-warden: `integrations/egress/fd4_broker.c` (server, 202 lines) and
`src/netclient/{protocol.h,client.c}` (client, 198 lines), read at 9a7c1f5. The
protocol they speak is private to Warden, and an integrator who wants the native
mode without Warden has nothing to build on but Warden's sources.

The channel should belong to Egress: the semantics it carries — exact
destinations, pinned addresses, the SNI guard, quotas, receipts — are Egress's,
and only Egress can promise them. This document proposes the boundary and the
contract. The criterion it serves: **an integrator uses the native mode across a
process boundary without importing maelys-warden or reading its sources.**

## Boundary

Egress owns:

- the channel protocol, versioned and documented here, later under `protocol/`;
- the server (the broker): it holds one authenticated connector and answers
  requests on one channel;
- a client small enough to link into a confined process, with no dependency on
  the Egress core — a separate archive, so a workload never links the proxy;
- an interprocess example, the conformance vectors and the adversarial tests.

The supervisor (maelys-warden, or any other) owns:

- creating the confined process and removing its other network paths;
- binding the channel to an execution identity: it creates the connector with
  the principal it chose, and the client never names one;
- handing the channel end to the process, and the descriptor number it lands on
  (Warden's convention is 4; this document never assigns a number);
- supervision, and what happens to the process when the channel ends.

The channel does not make Egress a sandbox. A workload with an ambient network
path ignores it; that the path is absent is the supervisor's property to
establish, not the channel's.

## Transport

Current: an `AF_UNIX` `SOCK_DGRAM` socket pair created with `CLOEXEC`; one
datagram per message; the stream travels as `SCM_RIGHTS` ancillary data on the
response.

Proposed: unchanged. A datagram carries message boundaries for free, which is
why the messages below have no length prefix, and a truncated datagram is
detectable (`MSG_TRUNC`, `MSG_CTRUNC`) where a truncated stream read is not.
The pair is created by the server; the supervisor receives the client end.

## Messages

All integers are big-endian. Offsets are in bytes.

### Request — client to server

| offset | size | field | value |
|---|---|---|---|
| 0 | 4 | magic | `0x4d454351` (`MECQ`) |
| 4 | 1 | version | `1` |
| 5 | 1 | protocol | `1` = TCP; no other value in v1 |
| 6 | 2 | port | `1`–`65535`; `0` is malformed |
| 8 | 2 | host_length | `1`–`253`; `0` and above `253` are malformed |
| 10 | host_length | host | the canonical host, see below |

A datagram shorter than 10 bytes, or whose length is not `10 + host_length`, is
malformed. `host` is a canonical DNS name or IP literal in the form Egress
policies use: lowercase, no trailing dot, no NUL, no byte outside printable
ASCII (`0x21`–`0x7e`). The server does not canonicalise on the workload's
behalf; a host that is not canonical is malformed. Egress validates the same
form again inside `session_open`, so a request the server lets through and the
policy refuses answers `DENIED`, not `PROTOCOL`.

Current: identical layout with magic `0x4d45584e` (`MEXN`); the server checks
port, length and bounds but not the host's bytes; NUL inside the host would be
cut by the C string the server builds.

### Response — server to client

| offset | size | field | value |
|---|---|---|---|
| 0 | 4 | magic | `0x4d454350` (`MECP`) |
| 4 | 1 | version | `1` |
| 5 | 1 | status | one code of the table below |
| 6 | 2 | reserved | `0`; a client ignores the value in v1 |

Ancillary data: on `OK`, exactly one descriptor in one `SCM_RIGHTS` header. On
any other status, none. A response of another size, another magic, with
ancillary data of another shape, or truncated, is malformed for the client.

Current: 8 bytes as `magic u32, status u32`, the status an `errno` of the
platform (`EACCES`, `ETIMEDOUT`, `ECANCELED`, `ENOMEM`, `EPROTO`, `EIO`), no
version field. `errno` values differ between Linux and macOS; that does not
break a local channel between two processes of one machine, but it makes the
contract depend on the platform and on how each side's C library numbers its
errors. A protocol carries its own codes.

### Status codes

| code | name | when |
|---|---|---|
| 0 | `OK` | the stream is attached |
| 1 | `DENIED` | the sealed policy refuses the destination |
| 2 | `TIMEOUT` | the upstream connect did not complete within the server's deadline |
| 3 | `CANCELLED` | the server is stopping |
| 4 | `MALFORMED` | the request violates this document |
| 5 | `UNSUPPORTED` | version or protocol the server does not speak |
| 6 | `RESOURCE` | the server could not allocate |
| 7 | `INTERNAL` | anything else; the server says more in its own log |

The codes are the protocol's, not a numeric mirror of `maelys_egress_result_t`.
The server maps by an explicit table — `ERR_DENIED → DENIED`, `ERR_TIMEOUT →
TIMEOUT`, `ERR_CANCELLED → CANCELLED`, `ERR_MEMORY → RESOURCE`, `ERR_ARGUMENT`
and `ERR_PROTOCOL → MALFORMED`, everything else including results that do not
exist yet `→ INTERNAL` — so the C API and the wire can move separately. A client
treats a code it does not know as `INTERNAL`.

## Decisions

Each decision states what the current Warden pair does, what v1 proposes, and
the conformance test that would hold the proposal.

### 1. Identity

*Current:* the broker is created with a connector the supervisor authenticated;
the request carries no identity. Implicit, unwritten.

*Proposed:* written. A channel is bound at creation to one connector, hence to
one principal and one invocation; the request has no field for either, and v1
adds none. A client that wants another identity needs another channel from its
supervisor.

*Test:* a request with trailing bytes beyond `10 + host_length` is answered
`MALFORMED`; there is no encoding by which a request selects a principal.

### 2. Descriptor ownership

*Current:* the server calls `session_take_fd`, sends the response, then closes
its copy of the stream and releases the session, whatever `sendmsg` returned;
the return value is discarded. There is no leak: the server's copy is always
closed. What is missing is that delivery is unconfirmed — the server cannot
tell a delivered stream from one the kernel never queued.

*Proposed:* the server checks `sendmsg`. On failure it closes its copy and
releases the session as today, and records the failure in its log; the client
end of the relay is then closed, and Egress treats the session as it treats any
client that closes before writing. What the receipt records for such a session
is Egress's receipt contract, not this document's: the channel promises no
receipt field. There is no acknowledgement message in v1: the client's successful `recvmsg` of an `OK`
response with one descriptor is the only confirmation, and a client that
receives nothing (EOF, error) treats the request as failed with no stream.
On the client side, the received descriptor is set `CLOEXEC` before anything
else is done with it; the kernel does not carry that flag across `SCM_RIGHTS`
on every platform (`MSG_CMSG_CLOEXEC` exists on Linux, not on macOS).

*Test:* the client end of the channel is closed while a request is in flight;
after the server's `sendmsg` fails, the process's descriptor count is what it
was before the request and the session count of the connector is zero. A
descriptor received by the client has `FD_CLOEXEC` set.

### 3. Bounds, deadlines and shutdown

*Current:* host at most 253 bytes; connect deadline a server constant of
5000 ms; the server thread ends on `POLLHUP`/`POLLERR` of the channel or on the
supervisor's wakeup.

*Proposed:* the host bound stays 253. The connect deadline is the server's,
set by the supervisor when it creates the channel server, bounded by Egress to
a finite non-zero value; v1 has no field for the client to set or extend it.
The exchange as a whole is bounded on both sides. The server answers every
request within the connect deadline plus a bounded processing time of its own
(reading the datagram, `session_open`, `sendmsg`); while the channel is open it
never leaves a request unanswered. The client applies a read deadline of its
own choosing, at least the server's connect deadline; when it expires, the
client **closes the channel** rather than sending another request on it, since
with one request in flight and no identifier a late response would be paired
with the wrong request. The server's deadline is not carried on the wire in
v1: the supervisor that configures it is the one that hands the channel over,
and tells the workload out of band, or the workload uses a generous bound. Two
closures are distinct:

- *the channel closes* — the client closed its end, or the supervisor destroyed
  the server. A request in flight completes inside Egress; its response is
  discarded and its stream closed. The channel's closure does **not** close
  streams already handed over: the connector API says releasing the connector
  does not close returned sessions, and the channel promises nothing stronger;
- *streams are revoked* — only by stopping the Egress server
  (`maelys_egress_server_stop` cancels pending opens and active sessions), or
  by the workload closing them. A supervisor that wants both closes the channel
  and stops the server.

*Test:* open a stream, close the channel, write through the stream: the bytes
arrive upstream. Stop the server: the stream reads EOF. Send a request, then
close the channel before the response: the server's session count returns to
zero within the deadline.

### 4. Serialisation

*Current:* one server thread reads one datagram at a time and answers it
before reading the next; the client sends one request and blocks on one
response. Requests are therefore ordered and, in practice, one in flight; a
client that sends two before reading gets two responses in order, with nothing
in the response that says which is which.

*Proposed:* one request in flight per channel, written. A client sends the next
request only after reading the previous response. The server keeps ordered
processing; it does not need to detect a second request early, since a
datagram waits in the socket. No request identifier in v1: multiplexing has no
requester, and an identifier is the v2 escape hatch if one appears.

*Test:* two requests sent back to back receive two responses in order, each
with its own stream or status; the vectors state this as the behaviour a client
must not rely on, since v1 only promises the sequential case.

### 5. Errors and version

*Current:* `errno` values; a request with an unknown version is answered
`EPROTO`, indistinguishable from a malformed one.

*Proposed:* the table above. The request's version byte is checked before
anything else: an unknown version answers `UNSUPPORTED` with the server's own
version in the response, so a client learns what to speak; an unknown protocol
byte likewise. Everything else malformed answers `MALFORMED`. A v1 client
receiving a response whose version is not `1` treats it as malformed.

*Test:* one vector per code, including version `2` → `UNSUPPORTED` with
response version `1`, and protocol `2` → `UNSUPPORTED`.

### 6. Descriptor cardinality and truncation

*Current:* the client's control buffer has room for four descriptors. It keeps
the first while counting and closes every further one, then rejects the
response as a protocol error unless exactly one arrived and every ancillary
header was a well-formed `SCM_RIGHTS`; on a non-zero status it closes whatever
arrived; it rejects `MSG_TRUNC` and `MSG_CTRUNC`. The cardinality below is
therefore what the current client already enforces; what changes is that the
server side states it too, and that a descriptor on a non-`OK` status becomes a
malformed response rather than a silently closed one.

*Proposed:* exactly one descriptor on `OK`, none otherwise. A client receiving
a descriptor with a non-`OK` status, more than one descriptor, an `SCM_RIGHTS`
header of another shape, or any ancillary data of another type, closes every
descriptor it received and treats the response as malformed. Truncation of
either the data or the ancillary part is malformed.

*Test:* vectors for zero, one and two descriptors on `OK`; one descriptor on
`DENIED`; `MSG_CTRUNC` provoked with a too-small control buffer.

### 7. What success means

*Current:* undocumented on the wire; the connector API says it.

*Proposed:* written in the protocol. `OK` means Egress has connected to a
pinned upstream address for the destination and has attached the client end
of **its own relay** — never the upstream socket. It does not mean the future
TLS ClientHello has passed the SNI guard, nor that later bytes will be relayed:
the guard runs on the first bytes, quotas run on every byte, and either closes
the stream; the receipt records what happened. A client infers nothing about
future traffic from `OK`.

*Test:* a policy with `allow_tls_sni` for the host; `OK`; a ClientHello with
another SNI; the stream closes and the receipt records the SNI refusal.

## Conformance vectors

To be produced under `tests/vectors/channel/` with the implementation, each as
hex plus the expected outcome: the valid request and response; every malformed
request of section "Messages"; every status code; the descriptor cardinality
cases; the sequential exchange of two requests. The same vectors drive the
server's tests here and any client that wants to claim conformance.

## Open questions, for maelys-warden first

1. Magics: new values (`MECQ`/`MECP`) so a current `netclient` and a channel
   server can never mistake each other, or Warden's values with a version bump
   to `2`? This document proposes new values, since the protocol changes owner.
2. The connect deadline: a server-creation parameter, as proposed, or a
   configuration key of the embedding side? The client has no say in v1 either
   way.
3. The client archive's name and licence header; it must link into a confined
   process without the Egress core.
4. Whether Warden's `netclient` becomes a thin wrapper of Egress's client or
   is replaced by it.

## Not in this document

CLI exposure. The repository's convention keeps operational settings in the
configuration file, so a channel mode of `serve` would be a key, not an option,
and receiving a descriptor from the launching environment is its own design;
that comes after the library surface and the example exist. The freeze of v1:
it comes after the implementation and its adversarial gate, not with this
text.
