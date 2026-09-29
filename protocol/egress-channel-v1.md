# Maelys Egress mediated-connection channel, v1

**Status: contract, version 1, frozen with Egress 0.22.0.** The bytes and the
decisions below are what `libmaelys_egress` serves
(`maelys_egress_channel_create`), what `libmaelys_egress_client` speaks
(`maelys_egress_client_connect`), what `tests/vectors/channel/` states and what
the mutation and fuzz gates hold. A change to a byte or to a decision is a
version 2, with its own document; this one no longer moves. It began as a
proposal reviewed with maelys-warden, whose answers to its four open questions
are recorded at the end.

## Why

`maelys_egress_connector_session_open` ([egress.h](../include/maelys/egress.h))
returns a policy-checked TCP stream to an embedder in the same process. A
sandboxed workload lives in another process; someone must carry its request
across that boundary and hand the stream back. Before this
contract that someone was maelys-warden alone, with a private pair of its own
(`integrations/egress/fd4_broker.c` and `src/netclient`, read at 9a7c1f5), and
an integrator who wanted the native mode without Warden had nothing to build on
but Warden's sources.

The channel belongs to Egress: the semantics it carries — exact destinations,
pinned addresses, the SNI guard, quotas, receipts — are Egress's, and only
Egress can promise them. The criterion this document serves: **an integrator
uses the native mode across a process boundary without importing maelys-warden
or reading its sources.** The pieces: the codec of
[`egress_channel.h`](../include/maelys/egress_channel.h), the server of
[`egress.h`](../include/maelys/egress.h), the client of
[`egress_client.h`](../include/maelys/egress_client.h) in its own archive, and
[`examples/channel_supervisor.c`](../examples/channel_supervisor.c) with
[`examples/channel_client.c`](../examples/channel_client.c).

## Boundary

Egress owns:

- the channel protocol, versioned and documented here;
- the server: `maelys_egress_channel_create` holds one authenticated connector
  and answers requests on one channel, one thread per channel;
- the client, `libmaelys_egress_client`, small enough to link into a confined
  process: no Egress core, and of maelys-system the one object that passes a
  descriptor, `fdpass.o`, so a workload never links the proxy, the loop, the
  threads or the files; `maelys-egress-client.pc` requires nothing (see
  "The client's one object of maelys-system" below);
- the interprocess examples, the conformance vectors, the fuzz target and the
  mutation gate.

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

An `AF_UNIX` `SOCK_DGRAM` socket pair created with `CLOEXEC`; one datagram per
message; the stream travels as `SCM_RIGHTS` ancillary data on the response. A
datagram carries message boundaries for free, which is why the messages below
have no length prefix, and a truncated datagram is detectable (`MSG_TRUNC`,
`MSG_CTRUNC`) where a truncated stream read is not. The pair is created by the
server; the supervisor receives the client end and hands it over.

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

A request carries no ancillary data. The server reads every descriptor a
kernel can deliver with one datagram, closes each of them before anything
else, and answers `MALFORMED` to a request that carried any, or whose control
data was truncated. Kernels differ here: Linux drops descriptors the receiver
has no room for and says so with `MSG_CTRUNC`; macOS installs them in the
receiver regardless and says nothing, so a server that asked for no control
data would keep them open, unnamed, and a confined process could fill its
descriptor table. *Erratum of 0.22.1:* 0.22.0 received requests without a
control buffer and leaked those descriptors on macOS; the bytes of the
protocol do not change, and a client that follows it never attaches any.

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

### Status codes

| code | name | when |
|---|---|---|
| 0 | `OK` | the stream is attached |
| 1 | `DENIED` | the sealed policy refuses the destination |
| 2 | `TIMEOUT` | the upstream connect did not complete within the server's deadline |
| 3 | `CANCELLED` | the server is stopping, or has stopped |
| 4 | `MALFORMED` | the request violates this document |
| 5 | `UNSUPPORTED` | version or protocol the server does not speak |
| 6 | `RESOURCE` | the server could not allocate |
| 7 | `INTERNAL` | anything else; the server says more in its own log |

The codes are the protocol's, not a numeric mirror of `maelys_egress_result_t`.
The server maps by an explicit table — `ERR_DENIED → DENIED`, `ERR_TIMEOUT →
TIMEOUT`, `ERR_CANCELLED` and `ERR_STATE → CANCELLED` (a server that is
stopping cancels the open, one that has stopped refuses it as a state; from
the channel both read the same), `ERR_MEMORY → RESOURCE`, `ERR_ARGUMENT` and
`ERR_PROTOCOL → MALFORMED` (a host the codec accepts but the connector refuses
as not canonical lands here), everything else including results that do not
exist yet `→ INTERNAL` — so the C API and the wire can move separately. A client
treats a code it does not know as `INTERNAL`.

## Decisions

Each decision keeps the shape of its review: what the pair that preceded this
contract did, what v1 does, and the test that holds it. "Current" names
maelys-warden's private pair at 9a7c1f5, kept for the record of why.

### 1. Identity

*Current:* the broker is created with a connector the supervisor authenticated;
the request carries no identity. Implicit, unwritten.

*Contract:* written. A channel is bound at creation to one connector, hence to
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

*Contract:* the server closes its copy of the stream and releases the session
whether or not the kernel took the datagram; the client end of the relay is
then closed when the send failed, and Egress treats the session as it treats
any client that closes before writing. The server's channel end is
nonblocking. If its response cannot be queued, the server closes the channel:
keeping it open would leave that request unanswered and a blocking send would
let a client that does not read prevent its supervisor from destroying the
channel. The server keeps no log of the refused send: the channel has no
logging surface, and none is promised. What the receipt records for such a
session is Egress's receipt contract, not this document's: the channel
promises no receipt field. There is no acknowledgement message in v1: the
client's successful `recvmsg` of an `OK` response with one descriptor is the
only confirmation, and a client that receives nothing (EOF, error) treats the
request as failed with no stream.
On the client side, the received descriptor is set `CLOEXEC` before anything
else is done with it; the kernel does not carry that flag across `SCM_RIGHTS`
on every platform (`MSG_CMSG_CLOEXEC` exists on Linux, not on macOS).

*Test:* the client end has shut its reading side before the answer comes, so
the kernel refuses the server's datagram every time (a close would race the
answer, and a datagram already queued is collected by the kernel later);
after the refused send, the process's descriptor count is what it was before
the request and no session is active. A descriptor received by the client has
`FD_CLOEXEC` set. A client that fills the response queue without reading sees
the channel close, and the supervisor destroys it before closing the client
end. (`tests/test_operations.c`, `tests/test_client.c`.)

### 3. Bounds, deadlines and shutdown

*Current:* host at most 253 bytes; connect deadline a server constant of
5000 ms; the server thread ends on `POLLHUP`/`POLLERR` of the channel or on the
supervisor's wakeup.

*Contract:* the host bound stays 253. The connect deadline is the server's,
set by the supervisor when it creates the channel server, bounded by Egress to
a finite non-zero value; v1 has no field for the client to set or extend it.
The exchange as a whole is bounded on both sides. The server answers every
request within the connect deadline plus a bounded processing time of its own
(reading the datagram, `session_open`, `sendmsg`); while the channel is open it
never leaves a request unanswered: failure to queue the answer closes the
channel. The client applies one absolute monotonic read deadline of its own
choosing, at least the server's connect deadline; interruptions consume that
same interval rather than restarting it. When it expires, the client **shuts
the channel down** rather than sending another request on it,
since with one request in flight and no identifier a late response would be
paired with the wrong request: a further call on that channel fails, and the
descriptor stays the caller's to close. Whether the server's end then reads
end of file differs between kernels for a datagram pair; the contract promises
the refusal, not the EOF. The server's deadline is not carried on the wire in
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
zero within the deadline. Interrupt a client wait repeatedly: it still ends at
its original deadline. Fill the response queue while keeping the client end
open: the server ends the channel and its destructor returns.

### 4. Serialisation

*Current:* one server thread reads one datagram at a time and answers it
before reading the next; the client sends one request and blocks on one
response. Requests are therefore ordered and, in practice, one in flight; a
client that sends two before reading gets two responses in order, with nothing
in the response that says which is which.

*Contract:* one request in flight per channel, written. A client sends the next
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

*Contract:* the table above. The request's version byte is checked before
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

*Contract:* exactly one descriptor on `OK`, none otherwise. A client receiving
a descriptor with a non-`OK` status, more than one descriptor, an `SCM_RIGHTS`
header of another shape, or any ancillary data of another type, closes every
descriptor it received and treats the response as malformed. Truncation of
either the data or the ancillary part is malformed.

*Test:* vectors for zero, one and two descriptors on `OK`; one descriptor on
`DENIED`; `MSG_CTRUNC` provoked with a too-small control buffer.

### 7. What success means

*Current:* undocumented on the wire; the connector API says it.

*Contract:* written in the protocol. `OK` means Egress has connected to a
pinned upstream address for the destination and has attached the client end
of **its own relay** — never the upstream socket. It does not mean the future
TLS ClientHello has passed the SNI guard, nor that later bytes will be relayed:
the guard runs on the first bytes, quotas run on every byte, and either closes
the stream; the receipt records what happened. A client infers nothing about
future traffic from `OK`.

*Test:* a policy with `allow_tls_sni` for the host; `OK`; a ClientHello with
another SNI; the stream closes and the receipt records the SNI refusal.

## Conformance vectors

`tests/vectors/channel/requests.txt` and `responses.txt`: one line per vector
as name, hex and expected outcome — the valid requests, every malformed and
unsupported request of section "Messages", every status code, the malformed
responses. `test-channel` replays them against the codec object alone and
round-trips every accepted one; `fuzz_channel` runs both decoders on every
input. A client or server that claims conformance replays the same lines. The
descriptor cardinality cases and the sequential exchange of two requests are
held by `tests/test_client.c` and `tests/test_operations.c`, since they need a
socket pair, not bytes alone.

## What maelys-warden answered

The four questions the proposal left open, and the answers of Warden's
operator that this version records:

1. Magics: new values, `MECQ`/`MECP`. The protocol changed owner, and a
   `netclient` of before must never be taken for a channel server. Warden's
   former pair stays as it was until its migration.
2. The connect deadline: a parameter of `maelys_egress_channel_create`.
   Warden creates the server for each execution and knows its budgets; the
   confined process has no say.
3. The client archive: `libmaelys_egress_client`, `maelys-egress-client.pc`,
   MPL-2.0 with an SPDX header in each file, linking without the Egress core
   and without maelys-system, so that a confined process inherits neither.
   Amended with Warden's agreement when maelys-system 0.10.0 published its
   descriptor-passing primitive: see the next section.
4. Warden's `netclient` becomes a thin wrapper of this client first, since it
   is part of the SDK Warden installs; it is retired in a later major version
   of Warden, with a documented migration.

## The client's one object of maelys-system

Until maelys-system 0.10.0 the rule for the client was "no maelys-system at
all", and the client and the server each carried their own `SCM_RIGHTS`
code. Two copies drifted, and one of them was the flaw of 0.22.0: a server
that received requests without room for their descriptors, which macOS then
installed in it unseen. maelys-system 0.10.0 publishes the gesture once —
`maelys_sys_fd_send` and `maelys_sys_fd_receive` in `maelys/sys/fdpass.h`,
with room for every descriptor either kernel delivers, the surplus closed and
flagged, close-on-exec set — and Egress adopts it on both sides, so that one
proven copy of descriptor passing serves the channel.

The rule is now, as maelys-warden agreed: **`fdpass` alone** — the client
includes `maelys/sys/fdpass.h` and names `maelys_sys_fd_send`,
`maelys_sys_fd_receive` and their result type, nothing else of maelys-system,
and the archive holds `fdpass.o` taken from the pinned `libmaelys_sys.a` —
**and no undefined `maelys_sys_` or `pthread_` symbol in the archive**. The
promise it keeps is the original one: a confined process that links the
client inherits neither the loop, nor the threads, nor the files of
maelys-system. Four guards hold it: `client-standalone-check` on the
archive's symbols; the archive linked alone, without the library and without
`-pthread`, by `test-client`, by the release smoke and by the Homebrew test;
the boundary audit on `client/`; and maelys-system's own
`fdpass-standalone-check`, which links the object by itself.

## Not in this document

CLI exposure. The repository's convention keeps operational settings in the
configuration file, so a channel mode of `serve` would be a key, not an option,
and receiving a descriptor from the launching environment is its own design;
it comes, if it comes, as a version of the command, not of this protocol.
