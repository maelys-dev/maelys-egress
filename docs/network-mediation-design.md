# Network mediation design — proxy reachability and the SNI guard

Status: living design and implementation record. This
document explains the shipped network-mediation mechanisms — the SNI guard
(egress 0.2.0), per-execution principals and `maelys_egress_profile` (0.3.0), TLS
listeners (0.4.0), the filesystem `AF_UNIX` listener (0.5.0), and operational
policy generations, quotas, metrics and durable receipts (0.6.0) — and
specifies the remaining fd-4 integration with Executor 0.13.0 and its fd 4
broker. Version numbers are those recorded in
[CHANGELOG.md](../CHANGELOG.md) and [docs/roadmap.md](roadmap.md); the Egress
renamed tree targets 0.9.0; the native connector and endpoint-bound principal
described here are delivered.

This document does two things. First, it explains — from the ground up — the
network-mediation mechanisms Egress already ships: how a confined workload
reaches the proxy and nothing else across sandbox backends, and how a small
guard on the TLS ClientHello closes an identity gap the sealed allowlist alone
cannot. Second, it specifies the remaining piece: adapting Executor's fd-4
request protocol to Egress's native connector so Egress — rather than a second
allowlist in Executor — is the single network-enforcement engine for both
proxy-aware and Maelys-aware clients. Executor 0.13.0 already delivers the
Bubblewrap relay/bootstrap and Seatbelt standard-proxy route.

It builds on the shipped surface rather than repeating it. The exact sealed
allowlist, pinned DNS addresses, the `CONNECT` / one-exchange HTTP forward /
SOCKS5 modes, the policy digest and receipts, the TLS provider seam and the
`maelys_egress_profile` environment seam are described in
[docs/architecture.md](architecture.md),
[docs/security-model.md](security-model.md), [docs/tls.md](tls.md),
[docs/roadmap.md](roadmap.md) and [README.md](../README.md). This document
references those mechanisms and does not restate their internals. Where a
mechanism is delivered it is described in the present indicative; forthcoming
pieces are marked as such in Sections 2 and 6.

The scope split is deliberate. Egress owns network admission, DNS pinning,
connection creation, protocol handling, the SNI guard and network receipts.
Executor owns sandbox construction, helper placement, launch ordering,
process lifetime and the per-execution binding between a SandboxPlan and a
Egress policy. `maelys-system` supplies only the low-level reactor, descriptor,
deadline and bounded-I/O mechanics. The optional adapter may link Executor and
Egress; neither core library becomes a dependency of the other.

## 1. The reachability problem

> 127.0.0.1 does not name a machine. It names a network stack.

Egress enforces an exact, deny-by-default allowlist on the destinations a
workload may reach. That enforcement is worth nothing if the workload can also
open sockets that never pass through Egress. So confinement has two halves that
must hold together:

- the workload must be able to reach the proxy; and
- the workload must be able to reach *nothing else* directly.

The difficulty is that "reach the proxy" is not a fixed address. A proxy
endpoint is a socket in some network stack. Whether the workload shares that
stack depends entirely on the sandbox backend. On one backend the workload and
the proxy sit in the same stack and `127.0.0.1:PORT` is literally the same
listener; on another the workload is placed in a brand-new, empty stack where
`127.0.0.1` is its own private loopback with nothing listening, and the host's
loopback is simply unreachable. The same string, `127.0.0.1`, denotes different
stacks in the two cases.

A design that treats the proxy endpoint as an address rather than as *a socket
in a specific stack* will either fail to connect or, worse, "fix" the failure
by reopening ambient network — which makes the allowlist decorative. The rest
of this section resolves reachability per backend without ever taking that
second step.

## 2. Three crossings, two frontends, one enforcement engine

The mediation route is chosen by both the backend and the client. The backend
decides whether the workload shares the proxy's network stack. The client
decides whether it can speak Maelys' fd 4 connect protocol or requires a
standard HTTP/SOCKS proxy endpoint.

| Backend | Client | Boundary crossing | Why nothing else is reachable | Status |
| --- | --- | --- | --- | --- |
| Seatbelt | standard HTTP/SOCKS | one allowed host-loopback TCP port | every other connect is denied by SBPL | D1 |
| bubblewrap | standard HTTP/SOCKS | private loopback relay to a bind-mounted Unix socket | the network namespace has no ambient route | D2, D3 |
| either | Maelys-aware | inherited Unix-domain fd 4, then `SCM_RIGHTS` | the workload receives a host-created connected socket | D13 |

These are three boundary mechanisms, not three policy engines. Egress must be the
single enforcement engine behind both frontends:

```text
                         +-- fd 4 connect broker --------+
workload                 |                               |
                         +-- HTTP/SOCKS, via relay -------+---> Egress policy core
                                                               |-- exact host/port
                                                               |-- DNS pinning
                                                               |-- connect
                                                               |-- optional SNI guard
                                                               `-- receipts
```

The fd 4 frontend accepts a structured connect request and returns a connected
socket. The HTTP/SOCKS frontend accepts a standard proxy protocol and relays
bytes. They do not need the same transport or parser, but they must consume the
same sealed policy generation, the same pinned endpoints and the same receipt
identity. Receipts offer the same guarantees on both paths but are not
literally identical: each records which frontend admitted the stream (`HTTP`,
`SOCKS` or `FD_BROKER`), so provenance is distinguished honestly rather than
flattened. Two entry paths are intentional; two independent allowlists,
resolvers or connection implementations are not.

The broker frontend has one benefit worth stating exactly. A structured
connect request carries a canonical name, so the workload performs no DNS of
its own, chooses no address directly, reaches no arbitrary resolver, and the
name declared to policy is the only name there is — resolution and admission
happen in Egress, before any TLS byte exists. That makes the broker the stronger
frontend for controlling DNS and the declared destination. It is not by itself
a solution to ECH: the workload may hold a previously obtained or embedded ECH
configuration, and the broker proves nothing about an encrypted inner name.
With the currently shipped passive guard, an exact-name policy refuses ECH on
both frontends (section 4). The controlled-origin split mode described there is
a property of the destination deployment, not a hidden advantage of fd 4.

### Current state and target state

Executor 0.13.0 implements an fd 4 broker. fd 4 is a blocking Unix
datagram control channel. A child sends a canonical `{protocol, host, port}`
request; Executor checks a copied destination list, calls `connect`, then sends
the connected socket back with `SCM_RIGHTS`. Egress is not involved in that
current path.

That implementation is a useful transport and compatibility contract, but it
must not remain a second enforcement engine once Egress is integrated. The
target adapter preserves the fd 4 request contract while delegating admission,
pinning, guarded relay and receipt production to a Egress connector. The socket
returned to the child terminates on a Egress-held relay peer rather than exposing
the upstream socket directly. The standard proxy path reaches the same Egress
policy through its HTTP/SOCKS listener. Section 6 defines this binding.

### Seatbelt: a surgical hole in a deny-default filter

macOS Seatbelt does not virtualize the network. The workload runs in the host's
network stack, so the host loopback listener *is* reachable in principle. The
confinement is a filter, not a partition: the SBPL profile is `deny default`
for network operations and then allows exactly the one loopback port Egress
listens on. Everything else — every other host, every other port, every other
loopback service — is a denied syscall at `connect` time.

```text
host machine, ONE shared network stack
+--------------------------------------------------------------+
|  sandbox-exec, Seatbelt profile                               |
|  +---------------------------------------------+             |
|  | workload                                    |             |
|  |   connect 127.0.0.1:PORT  ---> allowed      |             |
|  |   connect 140.82.x.y:443  ---> DENIED       |             |
|  |   connect 10.0.0.1:53     ---> DENIED       |             |
|  +---------------------------------------------+             |
|                    |                                         |
|                    v                                         |
|           maelys-egress, 127.0.0.1:PORT                        |
|                    |                                         |
+--------------------|-----------------------------------------+
                     v
        only the pinned addresses of the sealed policy
```

The hole is surgical: one allowed loopback port, and only outbound to it.

```text
(deny network*)
(allow network-outbound (remote tcp "localhost:PORT"))
```

The proxy's own upstream connections happen in Egress's process, which is not
under the workload profile. The workload can name `127.0.0.1:PORT` and reach
Egress; it can name anything else and the kernel refuses the syscall. Note the
character of that refusal: the packet *could* have been routed, and a filter
rejected it. Bubblewrap differs on exactly this point.

**D1 — on Seatbelt, Egress keeps its host loopback TCP listener and the generated
profile must allow exactly that one numeric loopback address and port.**
Nothing in Egress changes for this backend. Executor 0.13.0 ships the surgical
Seatbelt endpoint rule and its behavioural probe for the standard-proxy
frontend.

On Seatbelt, reachability is solved end-to-end for standard clients: Egress
ships a loopback TCP listener (`src/server_listener.c`, `AF_INET`/`AF_INET6`), and the
shared stack means the workload needs no relay and no new listener. The
Executor's behavioural probe proves all three properties together: the
selected numeric loopback endpoint is reachable, a neighbouring loopback port
is denied, and a non-loopback address is denied. The SBPL spelling alone is not
treated as proof.

### Bubblewrap: an empty stack, and a file that crosses into it

`bwrap --unshare-net` gives the workload a new network namespace: a fresh stack
with its own `lo` and its own `127.0.0.1`, on which nothing is listening. The
host's loopback — where a TCP Egress would listen — is in a different stack and
is unreachable from inside by construction. There is no route, no address, no
syscall that leaves the namespace over the network.

This is the feature, not an obstacle. The direct-connection refusal is
structural: it is a property of the empty stack, not a filter rule that could
be misconfigured or forgotten. Nothing to allow means nothing to get wrong.

The route in is not network at all. A Unix-domain socket is a file. Network
namespaces partition network stacks; they do not partition the filesystem. A
socket file bind-mounted into the sandbox is reachable across the namespace
boundary precisely because it is a file and the boundary is a network boundary.

```text
host network namespace              sandbox network namespace
+----------------------------+      +----------------------------+
|                            |      | fresh, EMPTY stack:        |
|  maelys-egress               |      | its own lo, its own        |
|    |                       |      | 127.0.0.1, nobody there    |
|    +- AF_UNIX listener ----+------+-> /run/maelys/egress.sock    |
|    |    (bind-mounted)     |      |         ^                  |
|    |                       |      |         | connect(path)    |
|    +- upstream TCP sockets |      |    in-namespace relay      |
|         (the real packets) |      |         ^                  |
|                            |      |         | TCP connect      |
|                            |      |    workload, HTTP_PROXY=   |
|                            |      |    http://127.0.0.1:PORT   |
+----------------------------+      +----------------------------+

  no route joins the two stacks. The socket file and inherited
  descriptors are the only objects that cross.
```

**D2 — on bubblewrap, `--unshare-net` is kept and Egress gains an `AF_UNIX`
listener.** This is the Egress half of standard-client reachability. Egress today
listens only on TCP (`src/server_listener.c` uses `getaddrinfo` over
`AF_INET`/`AF_INET6`; there is no `AF_UNIX` path), so this route does not exist
yet. The work is a Egress **addition** — an `AF_UNIX` listener alongside the
existing TCP one — plus an Executor-side component. With that listener in
place, Executor bind-mounts the socket path into the sandbox; a relay connects
to a path, not to an address, and the empty stack is never a problem because
the path is not routed — it is opened.

The listener speaks the same authenticated HTTP and SOCKS protocols as the TCP
listener. A client capable of selecting a Unix proxy endpoint can use it
directly. Ordinary tools — `curl`, `npm`, `git` — normally read a host-and-port
URL from `HTTP_PROXY` and cannot all dial a Unix socket, so they need one more
local mechanism.

**D3 — a bounded relay runs inside the namespace for tools that need a
host:port.** It listens on the sandbox's own `127.0.0.1` (the port announced in
`HTTP_PROXY`) and copies bytes to the bind-mounted Unix socket. The workload's
TCP loopback then has exactly one listener — the relay — and the relay's only
upstream is the file that crosses to Egress. The relay is an Executor-side
component, not part of Egress core; only the `AF_UNIX` listener it targets is the
Egress addition.

The relay carries no policy. It authenticates nothing, inspects nothing and
decides nothing; every check stays in Egress on the host side of the socket. Its
only security-relevant property is that it exists solely inside the namespace
and cannot widen it.

**D4 — the relay is an Executor-owned helper built on maelys-system. It is not
`socat`, it is not part of Egress core, and it is not part of maelys-system.**
Executor owns it because only Executor knows how to place a trusted binary in
the bubblewrap filesystem, start it inside the new network namespace, wait for
readiness before the workload starts, and stop it with that workload.
`maelys-system` supplies the reactor, socket, deadline, bounded-write and
idempotent-close mechanics; it acquires no knowledge of proxy protocols,
bubblewrap or sandbox policy. Egress owns only the host-side Unix listener and
the network decisions beyond it.

The relay is deliberately small in responsibility, not estimated by a source
line count. A release implementation must still provide bounded buffers,
backpressure, half-close propagation, a connection cap, descriptor and
environment hygiene, an explicit ready/failure handshake, bounded shutdown
and tests for client and Egress disappearance. It forwards bytes without parsing
HTTP or SOCKS and without possessing the allowlist.

### Who owns the bubblewrap bridge

The bridge crosses three ownership domains, and no one component owns all of
them:

| Object | Owner | Reason |
| --- | --- | --- |
| private host directory and sandbox mount | Executor bubblewrap backend | they exist only for one execution and are part of sandbox construction |
| `AF_UNIX` listener fd and socket node while active | Egress instance | Egress accepts and authenticates the proxy stream |
| in-namespace relay binary and process | Executor | its placement, readiness and lifetime are coupled to the workload |
| reactor, fd and bounded-I/O primitives | maelys-system | reusable mechanism with no sandbox or proxy semantics |

The launch is transactional:

1. Executor creates a private mode-0700 per-execution directory and chooses a
   path short enough for `sockaddr_un.sun_path`.
2. Egress binds an absent path, reports readiness, and remembers the created
   socket identity. Authentication remains mandatory; filesystem permissions
   are defence in depth.
3. Executor bind-mounts that socket, or its private directory, at a reserved
   read-only path in the bubblewrap filesystem. User mounts may not shadow the
   reserved path.
4. An Executor-owned bootstrap inside the new namespace starts the relay,
   waits for its explicit ready signal, then starts the workload. The workload
   receives proxy URLs naming only the relay's private-loopback port.
5. Shutdown stops and reaps the workload and relay, stops Egress, unmounts the
   namespace with the bubblewrap process, safely removes the socket only if it
   is the node Egress created, then removes the private directory.

Any failure before step 4 aborts the execution. There is no fallback to an
environment-only proxy configuration.

### The golden rule

> The hole in the wall must never itself be made of network.

For standard clients, every mediation route is either a filter exception to
one port (Seatbelt) or a socket **file** that crosses the boundary (bwrap). For
Maelys-aware clients, the crossing is an inherited Unix-domain descriptor.
None reopens ambient network.

**D5 — for a `MEDIATED` plan, sharing or restoring ambient network to make the
proxy reachable is forbidden; a backend that cannot prove exclusive mediated
reachability must refuse, not degrade.** Restoring an ambient route "so the
proxy is reachable" would make every other destination reachable too. The
allowlist would then constrain only
those programs that voluntarily honour `HTTP_PROXY`: every statically linked
binary, every runtime with its own transport, every `curl --noproxy` and every
deliberately hostile process ignores it. Worse, the receipts would become
fiction — they would record the connections that went through the proxy and say
nothing about the ones that did not. An adapter that cannot deliver exclusive
reachability for its selected backend fails the execution rather than launching
it with environment variables and calling that confinement. This restates, as a
mechanism requirement, what [docs/security-model.md](security-model.md) already
lists as a non-guarantee. This rule does not erase the distinct, explicitly
authorized `DIRECT`/`INHERIT` mode in Sandbox and Executor; it forbids silently
turning `MEDIATED` into that mode.

### The inherited fd: namespace-proof by construction

There is a third route, distinct from the relay. Executor installs one end of a
Unix datagram socketpair as child fd 4. That fd is a **control channel**, not a
pre-connected TCP socket. A Maelys-aware client sends a canonical connect
request on it. The host-side broker admits or denies the request, creates the
TCP connection on the host stack, and returns that new connected socket over
the control channel with `SCM_RIGHTS`.

The returned socket belongs to the stack that created it regardless of the
namespace in which the receiving process now lives. A namespace does not
reclassify an already-open fd; it governs creation and routing of new sockets.
The workload therefore never needs an ambient route and never opens the target
connection itself.

The fd route needs no relay and no filesystem-path listener: the inherited fd
4 is already the boundary-crossing capability. The relay exists only for
ordinary tools that insist on dialing a proxy at a host and port. The two
frontends are complementary, but after integration they terminate in the same
Egress policy and connector. Executor's current copied allowlist and direct
`connect` implementation are transitional, not a second long-term authority.

### What Egress actually does

Egress is not the sandbox, the network namespace or the in-namespace relay. It
is the host-side enforcement engine. For one immutable policy generation it:

1. canonicalises exact destination names and ports;
2. resolves each name at seal time, rejects disallowed address classes and
   pins the admitted addresses into the policy digest;
3. authenticates an HTTP/SOCKS principal or accepts an already-authenticated
   fd 4 request through the adapter;
4. matches the request against the sealed policy before creating a socket;
5. connects only to a pinned address under a deadline;
6. optionally requires an exact, bounded cleartext ClientHello SNI before
   forwarding tunnel bytes; and
7. emits a receipt containing the invocation and policy identities and the
   observed outcome.

HTTP parsing, SOCKS parsing and the fd 4 wire protocol are frontends. Exact
admission, pinned connection creation and receipts are the common core. Egress's
opaque connector/session seam now calls that core without exporting mutable
policy internals. The fd-4 wire decoder remains an adapter concern.

## 3. The SNI guard

The SNI guard ships today. It landed in egress 0.2.0 as an opt-in, bounded,
fragmentation-safe TLS ClientHello guard that requires one exact canonical SNI
before forwarding tunnel bytes, folds the SNI identity requirement into the
immutable policy digest, and records successful verification in the connection
receipt ([CHANGELOG.md](../CHANGELOG.md), 0.2.0). It is enabled per destination
with `allow_tls_sni = HOST:PORT` ([README.md](../README.md)). This section
explains why it is necessary and exactly what it does, because its necessity is
subtle; nothing here is forthcoming work.

### The CDN fact: an IP is not a site

`github.com:443` resolves to an IP address, but that address is a CDN front end
that hosts thousands of unrelated sites. The server does not decide which site
to serve from the IP the client connected to. It decides from the name the
client *pronounces* during the TLS handshake — the Server Name Indication (SNI)
— and presents the matching certificate and content.

The analogy is a large building. The street address (the IP) gets you to the
lobby, shared by thousands of tenants. Which apartment you are shown depends on
the name you give the concierge (the SNI), not on the address on the door. One
address, many tenants, and the name spoken at the desk selects among them.

### The gap an unguarded tunnel leaves

The name in an HTTP `CONNECT` (or a SOCKS5 destination) is a **declaration**.
The client asserts "I want github.com:443"; asserting it costs nothing, and a
client is free to lie. An unguarded destination checks that declared name
against the sealed allowlist, pins the resolved IP, opens the tunnel — and then
relays opaque bytes. After the tunnel is open, Egress sees ciphertext, not names.
This is the gap the guard closes; on a destination without `allow_tls_sni` it
remains open by design.

So a workload can declare `github.com:443`, pass the allowlist, and then run
its *own* TLS inside the tunnel with `server_name = evil.example`. If
`evil.example` is co-hosted on the same CDN IP that `github.com` resolved to,
the CDN happily serves it. In seven steps:

```text
1. workload  -> egress    CONNECT github.com:443
2. egress                 in the sealed allowlist. Admit.
3. egress      -> CDN     TCP connect to a PINNED address of github.com.
                        Legitimate: the address really is github's.
4. egress      -> workload  HTTP/1.1 200 Connection Established
                        from here the tunnel is opaque bytes to egress
5. workload  -> CDN     ClientHello, server_name = evil.example
                        (a tenant on the SAME CDN, so the SAME
                        address answers for it)
6. CDN                  routes BY NAME, not by address. Serves the
                        attacker's tenant. TLS completes normally.
7. workload  -> CDN     POST /collect, workspace in the body.
```

Step 5 is the pivot; everything before it is honest, and Egress's information
runs out at step 4. Egress's receipt says `host=github.com port=443` with a byte
count — a true statement about the TCP connection and a completely misleading
statement about where the data went. That is what makes this worth a guard
rather than a note: it does not merely bypass the allowlist, it turns the audit
trail into an alibi. The allowlist matched a promise the workload never had to
keep. [docs/security-model.md](security-model.md) states the residual plainly:
"An unguarded `CONNECT` remains an opaque TCP tunnel."

### Why classic TLS SNI is readable

The guard is possible because an ordinary TLS ClientHello without ECH carries
SNI in cleartext. The ClientHello is the *first* message of that handshake and
the server needs the name before it chooses a tenant certificate. This is the
historic virtual-hosting design that the shipped guard inspects; it is not a
logical law of all TLS. ECH deliberately changes the construction by using a
configuration learned before this handshake and encrypting an inner
ClientHello. Section 4 therefore treats ECH as unreadable rather than trying to
explain it away.

### The guard

**D6 — a destination may require that the first bytes of its tunnel be a
well-formed ClientHello naming exactly that destination; until that is proven,
not one byte is forwarded upstream.** Egress reads the first bytes of the tunnel
— the ClientHello, in the clear — extracts `server_name`, and compares it to
the name from the `CONNECT`. On a mismatch the connection is killed before any
application byte flows.

```text
CONNECT admitted            upstream connect succeeded
        |                             |
        v                             v
   POLICY MATCH  ----------------> IDENTITY
                                      |
                first client bytes accumulated, bounded at 16 KiB
                                      |
              +-----------------------+-----------------------+
              |                       |                       |
        incomplete              exact match             anything else
              |                       |                       |
        keep buffering        tls_sni_verified = 1        ERR_DENIED,
        (deadline applies)            |                   closed, receipt
                                      v                   emitted
                                    RELAY
```

What the parser in `src/clienthello.c` requires:

- The first flight must be a handshake record (`0x16`) of a TLS major version,
  and the first handshake message must be a ClientHello. So the guard doubles
  as a proof that the tunnel really is TLS.
- Records are reassembled across fragments up to `EGRESS_HANDSHAKE_MAX` (16 KiB),
  because a ClientHello can legitimately be split across records and a guard
  that inspected only the first record would be evaded by fragmenting it.
- Every length is checked against the bytes remaining: session ID, cipher
  suites, compression methods, the extension block and each extension body. The
  extension block length must equal the remainder exactly.
- Exactly one `server_name` extension, holding exactly one entry, of exactly
  type `host_name`. "Exactly one" matters: a parser that accepted the *first*
  matching name would be defeated by sending the allowed name first and the
  real one second.
- The name is canonicalised with the same `egress_canonical_host` used on the
  policy, then compared for exact equality. No suffix matching, no wildcards:
  `github.com` does not admit `github.com.attacker.example`.
- Encrypted ClientHello (extension `0xfe0d`, and the earlier `0xffce`) is
  refused outright — see §4.

This involves zero decryption, zero certificate handling, zero secret observed.
The guard reads one field that is already cleartext by design; it never
advances past key establishment and never possesses anything it could decrypt.
The shipped implementation fails closed on missing, duplicate, malformed and
mismatched SNI ([docs/security-model.md](security-model.md)), and records the
verified identity in the receipt and policy digest. Because
`egress_policy_compute_digest` includes the per-destination SNI requirement, two
policies differing only in whether the guard is on have different digests, and
a receipt's digest says which one was enforced.

The reason the guard actually closes the attack, rather than merely inspecting
traffic, is a dilemma it forces on the workload: **to reach `evil.example` the
workload must pronounce `evil.example` in the SNI**, because that is the only
name the CDN will route on — and pronouncing it is exactly what the guard sees.
The workload cannot both reach the disallowed tenant and hide which tenant it
reached. The lie that was free in the `CONNECT` declaration is no longer free
in the ClientHello.

### Tunnel and TLS are successive, not nested

A common confusion is to imagine TLS "inside" an encrypted tunnel, two layers
of crypto. There is only one. The `CONNECT` establishes **no** encryption: it
is a byte pipe. The TLS session then starts from zero *within* that pipe, with
its own cleartext ClientHello as its first flight. The guard reads that first
flight and stops; it never needs to see past key establishment, and there is no
outer encryption obscuring the ClientHello from it. Tunnel setup and TLS
handshake are sequential phases on the same byte stream, not concentric
envelopes.

## 4. Residuals and limits

The guard is narrow on purpose. Its limits are stated here rather than implied.

### Domain fronting

A more sophisticated attacker pronounces `github.com` in the SNI — passing the
guard — and then, inside the encrypted HTTP session, sends `Host:
evil.example`. The SNI and the HTTP Host disagree; the guard only sees the SNI,
so it is satisfied. This is domain fronting, and it was historically effective.

Two things bound it. The guard closes the cheap version: the attacker can no
longer simply lie in the SNI. The expensive version is closed, where it is
closed at all, **at the CDN** — major providers moved against domain fronting
from 2018 onward, and several now reject requests whose inner authority
disagrees with the negotiated SNI. That mitigation belongs to the CDN, is not
uniform across providers, and can be withdrawn without notice, so it must not
be read as a Egress guarantee. Egress shuts the inexpensive door and says so; it
does not claim the other one.

The same gap has a second shape worth naming: an HTTP/2 client that names the
sealed host in the SNI and then opens a stream for a different `:authority` on
a connection the origin is willing to coalesce. A cleartext ClientHello cannot
attest to anything that happens after key establishment.

### ECH (Encrypted ClientHello)

ECH splits the ClientHello in two. The readable *outer* hello carries a cover
name; the real destination travels in an *inner* hello sealed to the
client-facing ECH server's public key. The guard reads the outer name, which is
not the name that selects the site.

```text
Without ECH — one hello, one name, readable at the door

  ClientHello
    server_name: "github.com"        <- cleartext, and it is the name that
                                        selects the site. The guard reads it,
                                        compares it, admits or kills.

With ECH — a cover name outside, the real name sealed inside

  Outer ClientHello
    server_name: "public-front.example"
                                     <- cleartext, but it is the ECH config's
                                        public name, shared by the origins in
                                        that ECHConfig anonymity set
    extension encrypted_client_hello:
      +-------------------------------------------+
      |  Inner ClientHello           (HPKE-sealed) |
      |    server_name: "evil.example"             |
      +-------------------------------------------+
                        ^
                        |  sealed to a public key whose private half is held
                        |  by the client-facing server. A passive relay is not
                        |  a party to that exchange. A cooperating deployment
                        |  may make Egress that client-facing server (D14).
```

The last line is the whole difficulty, and it must be stated precisely. No
passive or on-path capability — including possession of a local MITM CA alone —
reveals the ClientHelloInner. The only way to read it is to be deployed as the
client-facing ECH service holding the corresponding private key, which RFC
9849 defines as belonging to the client-facing server
([RFC 9849](https://www.rfc-editor.org/rfc/rfc9849.html),
[RFC 9848](https://www.rfc-editor.org/rfc/rfc9848.html)). Reading ECH from an
ordinary relay position is therefore impossible: the required capability is
custody of the ECH private key selected by the configuration used by the
client.

That statement must not be confused with application-TLS interception. RFC
9849 explicitly permits the client-facing server and backend to be physically
separate. In this **ECH split mode**, the client-facing server decrypts the
ClientHelloInner and forwards it to the backend, then forwards the remaining
TLS messages unmodified. It learns the inner ClientHello — including the real
SNI — but does not necessarily learn application plaintext or mint a
certificate for the workload. This is narrower than the full MITM refused by
[D7](#5-what-this-design-refuses).

```text
Controlled-origin ECH split mode

  workload                Egress client-facing service        origin backend
     | ClientHelloOuter              |                            |
     |------------------------------>|                            |
     |                               | HPKE-decrypt inner SNI     |
     |                               | apply exact-name policy    |
     |                               |--------------------------->| Inner hello
     |<==============================|============================>| TLS records
                                     |
                                     `-- sees the inner handshake,
                                         not application plaintext
```

Split mode is nevertheless not a universal egress mechanism. The destination
operator must publish an ECHConfig whose public key belongs to the
client-facing service, route the connection through it, and run a cooperating
backend capable of the split protocol. Egress cannot substitute its own key into
the ECHConfig of `github.com`, a public CDN, or any other unrelated third
party. It is a real solution for domains the deployment controls or whose
operator cooperates; it is no solution for arbitrary destinations.

It follows that no refinement of a *passive* guard can do better. ECH's threat
model is the on-path observer that filters by name — a read-based guard is
exactly the adversary the protocol was designed to defeat. For an unrelated
destination, an exact-name policy must therefore refuse ECH or adopt a more
powerful interception model. For a controlled, cooperating destination, split
mode supplies a fourth, narrower posture: terminate ECH only, prove the inner
name, and leave the rest of TLS opaque.

The shipped guard therefore refuses any ClientHello carrying the ECH extension
on guarded destinations (`src/clienthello.c`, extension `0xfe0d` and the older
`0xffce`; [docs/security-model.md](security-model.md): "ECH is refused by
guarded destinations because inner SNI cannot be observed").

Refusal has a compatibility cost that must be named rather than hidden. Many
mainstream clients send a GREASE ECH extension — a decoy payload, sent without
using ECH, precisely so that real ECH does not stand out. GREASE is designed to
be indistinguishable from real ECH by inspection, so the guard cannot exempt
it: a client that presents the extension toward a guarded destination is
refused whether it meant it or not. This is the fail-closed direction — an
untrusted workload has no legitimate need to hide a destination name from its
own confinement — but it is a real cost, and it is why refusal is the
backstop of the answer, not its substance.

#### Why owning the resolver is not the answer

The sandbox has a lever an ordinary on-path observer lacks: it can own the
workload's resolver. The temptation is to conclude that this settles ECH, since
the ECH public key is published in the destination's DNS record. It does not,
because DNS is one delivery path among several:

```text
  DNS HTTPS/SVCB answer ------+
  compiled into the binary ---+
  a configuration file -------+--> ECH config held by the client --> sealed
  cached from an earlier run -+                                      inner
  fetched over an already-    |                                      hello
  allowed destination --------+

  only the first arrow passes through a controlled resolver
```

A workload holding a config by any of the other four paths performs no lookup
at the moment that matters. It opens a connection to an address the allowlist
already pinned and seals an inner name the mediator never sees. Resolver
isolation is real defence in depth — it constrains ordinary name discovery,
denies hardcoded public resolvers when ambient egress is absent, and records
what it serves — but it is **not proof of the encrypted inner name**, and it
cannot on its own justify accepting ECH for an exact-name policy.

#### What Egress can actually do

The available postures depend on who controls the destination. Each row names
the identity it actually delivers; none should be silently substituted for
another.

| Policy | Identity delivered | Destination scope | Status |
| --- | --- | --- | --- |
| Refuse ECH-bearing ClientHellos | no ECH session admitted; exact client-declared SNI for admitted non-ECH sessions | arbitrary | shipped |
| Accept on an operator-attested exclusive endpoint | endpoint `{IP, port}` only — no proof of the inner name | arbitrary or controlled | proposed, not built |
| Act as an RFC 9849 split client-facing server | exact client-declared inner SNI for admission; application TLS remains end-to-end opaque | controlled or explicitly cooperating origins only | deferred design, [D14](#9-decisions) |
| Intercept the complete TLS connection as a MITM | exact name and application content at the interceptor | clients trusting the injected CA | refused by [D7](#5-what-this-design-refuses) |

**Withholding ECH configs at a controlled resolver is a compatibility tactic
above refusal, not a posture.** A resolver should remove only the `ech`
SvcParam while preserving the other useful `HTTPS`/`SVCB` service parameters
where its implementation permits that safely. A compliant client that learns
no ECHConfig sends a readable ClientHello, so a hard failure becomes normal
operation for the common case. This guarantees nothing: the four non-DNS
delivery paths above bypass it, RFC 9849 allows servers to hand a rejected
client fresh `retry_configs` in the handshake itself, and changing a signed
answer can fail against a stub that validates DNSSEC strictly — the RFC
explicitly counts DNSSEC as protection against config injection *and removal*
([RFC 9849](https://www.rfc-editor.org/rfc/rfc9849.html)).

**The second posture must be stated as what it is: a change of identity basis,
not a proof.** A pinned address is a network endpoint, never by itself an
application identity. It becomes an acceptable trust basis only through an
external assertion:

> The operator attests that this `{IP, port}` set routes only to this
> destination for the lifetime of the policy.

Egress does not prove exclusivity, and — this is the part that must not be
overclaimed — resealing does not confirm it. Pinned-DNS resealing detects
*address-set changes*; it cannot detect a *tenancy change behind an unchanged
endpoint*:

```text
before      203.0.113.10:443 -> api.internal.example
after       203.0.113.10:443 -> api.internal.example
                             -> evil.example          (added by the provider,
                                                       no A/AAAA record moved)
reseal      203.0.113.10:443 -> the very same address set; nothing to see
```

A changed address set must invalidate the attestation; an unchanged one does
not confirm it. The asymmetry is the contract. A policy that opts in accepts
**endpoint identity instead of name identity** — the receipt records
endpoint-based admission without name proof, and the attestation is reviewed
on the operator's schedule, not discovered by Egress.

Because a boolean `allow_ech` would blur exactly this distinction, the future
API should force the identity basis to be chosen by name:

```c
typedef enum {
    MAELYS_EGRESS_TLS_IDENTITY_CLEAR_SNI_EXACT,
    MAELYS_EGRESS_TLS_IDENTITY_OPERATOR_ATTESTED_ENDPOINT,
    MAELYS_EGRESS_TLS_IDENTITY_UNVERIFIED
} maelys_egress_tls_identity_mode_t;
```

The mode enters the policy digest, the Executor binding record, every receipt
and the diagnostics, so an audit line can say what was actually established:

```json
{ "tls_identity_basis": "operator_attested_endpoint",
  "tls_name_verified": false }
```

One tactic is worth naming in order to reject it: verifying the *outer* SNI
against the config's advertised public name. It looks like a guard and is not
one — RFC 9849 itself notes that authenticating the public name does not
authenticate the origin — and all origins in the same ECHConfig anonymity set
can share that public name, so the check passes for exactly the origins it
would need to separate.

The layers compose rather than replace each other: pinned egress constrains IP
destinations, resolver isolation constrains ordinary name discovery, and the
SNI guard proves the name in a readable ClientHello. None reads an encrypted
inner name. For arbitrary third-party destinations under an exact-name policy,
the shipped fail-closed answer remains refusal; the only non-intercepting
fallback is the explicit downgrade to attested-endpoint identity above. For
controlled or cooperating origins, ECH split mode can prove the inner name
without exposing application content, but it requires a separate key,
protocol and backend design. Controlled DNS alone is insufficient.

### The tunnel stays opaque

The guard validates the *intent to open* — the name at the door — not the
content that flows afterward. Once keys are established the tunnel is
ciphertext and Egress relays it blind, as
[docs/security-model.md](security-model.md) records. The guard is an admission
check on identity, never a content inspector.

## 5. What this design refuses

**D7 — no TLS interception (MITM).**

Terminating the workload's TLS — presenting an injected certificate and
decrypting everything — would let Egress read full URLs and Host headers, closing
domain fronting directly. It would also mean Egress sees every secret in the
clear, and requires imposing a foreign CA into the workload's trust store so
the injected certificates are accepted. That is a large, adversarial
capability: a protected CA and key lifecycle, certificate generation and SAN
correctness, trust-root installation and removal, upstream chain verification,
explicit policy and audit for decrypted content, and correct handling of
pinning, mTLS, ECH and non-HTTP protocols ([docs/tls.md](tls.md)).

The SNI guard obtains one narrow guarantee — *the cleartext ClientHello name
equals the name allowed* — without any of that power. It does not prove the
peer certificate or the encrypted HTTP authority, as §4 makes explicit. It
sees no secret, installs no CA, generates no certificate, and decrypts nothing.
A local CA that can mint any certificate the workload will trust is a more
dangerous object than this design accepts merely to close the remaining gaps.

MITM therefore has no release number here. It is bound to the roadmap line it
already lives under: MITM, transparent interception and content inspection
"have no release number until their PKI and threat-model design is accepted
adversarially" ([docs/roadmap.md](roadmap.md)). This document does not advance
that line; it deliberately stays on the non-terminating side of it.

This refusal is specifically about terminating the workload's application TLS,
injecting trust roots, minting certificates and reading content. It does not
silently forbid RFC 9849 split mode: a split client-facing service decrypts the
ClientHelloInner but forwards the remaining TLS exchange unmodified to a
cooperating backend. That narrower capability has different key custody,
routing, denial-of-service and audit requirements, works only for controlled
or cooperating origins, and is therefore deferred separately as D14. It must
not be smuggled into the existing SNI guard or TLS-listener provider seam.

## 6. The Executor→Egress binding

The integration belongs in a separate adapter that links both public APIs.
Egress core does not include Executor headers; Executor core does not include
Egress headers. The adapter is allowed to know both worlds and owns the binding
lifetime.

The referenced systems in §7 commonly treat an allowlist as a configuration
file. Maelys has a stronger upstream chain: a portable decision and a resolved
plan admitted under that decision. The adapter derives the network policy from
that plan; it never reparses an unrelated policy file and never silently
widens an unsupported decision.

**D8 — one immutable logical Egress binding belongs to exactly one mediated
execution.** A binding contains the sealed policy generation, one
execution-scoped credential, the invocation identity, the Egress connector and
any proxy listener used by that execution. Its lifetime is bounded by the
execution:

1. The adapter copies exact destinations from the resolved SandboxPlan into a
   fresh Egress policy and seals it. Egress resolves once, rejects forbidden
   address classes and includes the pinned set in the policy digest.
2. The adapter creates one logical binding and one high-entropy
   execution-scoped credential. The credential is reusable for connections
   during that execution; it is not inaccurately called "single-use". It is
   never put in argv or receipts and is cleared when the binding is destroyed.
3. For standard clients, the adapter creates a Egress HTTP/SOCKS listener and
   applies `maelys_egress_profile`. The resulting `HTTP_PROXY`, `HTTPS_PROXY`,
   `ALL_PROXY` and lowercase entries are profile-produced environment
   augmentation; `NO_PROXY` and `no_proxy` remain cleared.
4. For Maelys-aware clients, the adapter supplies the host side of fd 4 and
   routes its structured connect requests into the same logical binding.
5. Only after Egress and any required relay report readiness does Executor launch
   the workload. Teardown is bounded and invalidates both credential and
   listener before the binding is released.

The initial implementation may use one sidecar process or one in-process Egress
server object per execution. That is a deployment choice, not part of the
security contract. The invariant is one immutable logical policy binding per
execution, not one operating-system process per execution.

**D10 — no shared mutable policy or credential namespace in v1.** A future
daemon may host several isolated server objects in one process, but a
connection must select an immutable binding before policy lookup, and one
binding's failure, credential or receipt stream must not cross into another.
Egress 0.6.0 can atomically replace one server's policy for future admissions;
that is generation replacement, not multi-binding multiplexing. A connection
still cannot select among several simultaneous policy/credential namespaces,
so multiplexing is explicitly out of v1 rather than forbidden forever. Any
future implementation is admitted only after measurements justify it and
adversarial tests prove cross-binding isolation.

### fd 4 and full Egress semantics

**D13 — two frontends terminate in one Egress enforcement core.** Merely replacing
Executor's allowlist check with a Egress allowlist check is insufficient if the
upstream socket is then handed directly to the workload. After that transfer,
Egress cannot inspect the first ClientHello, count relayed bytes or observe the
close, so it cannot claim the same SNI guard or receipt semantics as the proxy
frontend.

The full-semantics fd 4 path therefore returns a connected **client-side relay
stream**, not Egress's upstream socket. Egress retains the peer stream, performs the
pinned upstream connect and keeps the connection in its normal guarded relay
state. Because Executor 0.13.0 currently documents the returned object as a TCP
socket, the compatibility-preserving construction is a host-stack loopback TCP
pair: the client end crosses with `SCM_RIGHTS`, Egress retains the accepted end,
and no listener is exposed to the sandbox. An AF_UNIX `socketpair` would be
simpler but would require explicitly relaxing that public socket-kind contract.

```text
child request on fd 4
       |
       v
adapter / fd 4 frontend -- exact admission --> Egress policy generation
       |                                             |
       | client end via SCM_RIGHTS                   | pinned upstream connect
       v                                             v
workload socket <======== Egress guarded relay =====> destination
```

This keeps the fd 4 request format and namespace-proof delivery while preserving
SNI enforcement, byte accounting and close receipts. A reduced mode that hands
out the real upstream socket may exist only under a separately named capability
whose receipt states that post-connect traffic is unobserved; it is not the
default `MEDIATED` contract.

### Digest binding

**D9 — an explicit binding record joins the digest chain.** Independent
receipts carrying coincidentally correlatable values do not by themselves weld
the chain. Before launch, the adapter creates an immutable record containing at
least:

```text
invocation_id
decision_digest
executor_plan_digest
egress_policy_digest
executor_backend
egress_version
```

The Executor receipt retains that binding identity. Every Egress receipt contains
the same invocation ID and Egress policy digest, and names the frontend that
admitted the stream. No credential or proxy URL appears in either receipt. Egress
receipts remain observations rather than signed attestations, but the exact
policy selected for the execution is no longer an external inference.

### Mode and frontend are orthogonal on the Executor side

Executor's current network vocabulary folds a transport into the policy enum:
`EXTERNAL_CHANNEL` sits beside `INHERIT` and `NONE` as if the fd 4 broker were
a policy decision. It is not — it is a presentation of the same `MEDIATED`
decision. The migration separates the two, and removes `EXTERNAL_CHANNEL`
outright rather than keeping a deprecated value:

```text
network mode (policy)          network frontend (presentation)
  INHERIT                        NONE
  NONE                           STANDARD_PROXY
  MEDIATED                       FD_BROKER
                                 BOTH
```

The MIR stays pure — it says `MEDIATED` and names destinations, nothing else.
The Executor profile selects the frontend, exactly as the host context selects
the mediator. The rule that keeps the surface minimal: a frontend that was not
requested is not installed. `STANDARD_PROXY` alone plants the proxy
environment and, under bubblewrap, the relay — and no fd 4. `FD_BROKER` alone
installs the broker descriptor — and no proxy environment. `BOTH` serves a
mixed process tree from one sealed Egress policy. An idle fd 4 in a
standard-proxy workload would be surface without a consumer, and the converse
would be environment variables no client reads.

### Optional policy refinements

**D11 — HTTP forward-method restriction is a risk reducer, not an
anti-exfiltration boundary.** A destination may admit only `GET`/`HEAD` (or an
explicit set) in plaintext forward mode. This blocks ordinary `POST`/`PUT`
bodies, but data can still travel in a URL, query string or header. It has no
effect inside an encrypted `CONNECT` tunnel, where enforcing methods would
require the TLS termination D7 refuses. The method set therefore belongs in
the policy digest and receipt, but must never be described as closing all
exfiltration to an allowed host.

**D12 — prepare/run network profiles are desirable but deferred.** Setup and
agent execution have different risks, but Executor backend `prepare` compiles a
launch artifact; it is not a user-visible setup-script phase. Likewise,
`maelys_egress_policy_reseal` creates a fresh sealed policy, while Egress 0.6 can
install it atomically with `maelys_egress_server_replace_policy`. Existing
streams retain a copied destination and their original policy digest; only
future admissions observe the new generation.

A correct two-phase design needs an explicit higher-level phase boundary and
one of: two non-overlapping Egress bindings with credential rotation, or the 0.6
atomic generation switch. The mechanism now defines open-stream behaviour, but
the higher-level phase boundary and its authorization remain outside Egress.

## 7. Prior art

Two public projects solve adjacent problems. Both are cited by name and with
links, because this design converges with one on topology and diverges from the
other on defaults, and a reader deserves to check that.

### Anthropic sandbox-runtime

<https://github.com/anthropic-experimental/sandbox-runtime>

Its documented architecture states, for macOS, that "The Seatbelt profile
allows communication only to a specific localhost port. The proxies listen on
this port, creating a controlled channel for all network access." For Linux:
"The network namespace of the sandboxed process is removed entirely, so all
network traffic must go through the proxies running on the host (listening on
Unix sockets that are bind-mounted into the sandbox)", with `socat` bridging
the socket. It mediates "Both HTTP/HTTPS (via HTTP proxy) and other TCP traffic
(via SOCKS5 proxy)", which "enforce your domain allowlists and denylists",
points tools at the proxies through `HTTP_PROXY`, `HTTPS_PROXY` and
`ALL_PROXY`, and denies by default: "all network access is denied by default.
You must explicitly allow domains." TLS interception is opt-in, and when
enabled "HTTPS CONNECTs are terminated in-process", with a generated CA added
alongside the host's regular roots.

The topology in §2 is the same topology, reached from the same constraint. That
is a good sign rather than a problem: two independent designs facing the
Seatbelt/namespace asymmetry reach the same two answers because there are only
two answers. The differences worth recording: Egress is C11 over a pinned reactor
with no language runtime and links no TLS library in its default binary; Egress
authenticates the listener, which is what makes §6 possible; Egress pins resolved
addresses into the sealed policy and the digest, so a receipt names the
endpoints actually enforced rather than only the names configured; Egress refuses
interception rather than offering it (§5); and the relay is written in-house
rather than delegated to `socat` (D4).

### OpenAI Codex

<https://github.com/openai/codex> —
cloud: <https://learn.chatgpt.com/docs/cloud/internet-access> — configuration:
<https://learn.chatgpt.com/docs/config-file/config-reference> — sandbox:
<https://learn.chatgpt.com/docs/agent-approvals-security>

The **local CLI** confines commands with Seatbelt via `sandbox-exec` on macOS
and `bwrap` with `seccomp` on Linux, and documents that "By default, the agent
runs with network access turned off." Network access in the `workspace-write`
sandbox is the boolean `sandbox_workspace_write.network_access` — "Allow
outbound network access inside the workspace-write sandbox." A filtering proxy
does exist behind `features.network_proxy`, documented as "off by default",
with a `domains` map of `allow`/`deny` rules that is "Unset by default, which
means no external destinations are allowed until you add `allow` rules", a
loopback HTTP listener defaulting to `http://127.0.0.1:3128`, a SOCKS5
listener, an allowlist for Unix socket destinations, and an explicit
`dangerously_allow_non_loopback_proxy` guard on binding off loopback. The
reference is explicit that "permission-profile domain rules are not enforced
while the proxy is off."

Both readings are useful. Where the proxy feature is enabled it converges on
the same shape as §2 — deny-by-default domain policy, loopback listener, SOCKS5
alongside HTTP, Unix socket destinations as a separate allowlist, non-loopback
binding gated behind a name that says it is dangerous. But the default remains
boolean: with the proxy off, turning network access on grants ambient outbound
access to the confined command. That default is the model the golden rule
rejects for a `MEDIATED` plan. Maelys retains a separately named
`DIRECT`/`INHERIT` decision; what it forbids is silently implementing
`MEDIATED` as ambient access plus proxy environment.

The **cloud** product is more directive and supplies the two ideas in §6. It
documents that "By default, Codex blocks internet access during the agent
phase" while "Setup scripts still run with internet access so you can install
dependencies", offers per-environment domain allowlists, and offers restricting
"network requests to `GET`, `HEAD`, and `OPTIONS`". It states the threat
directly: enabling agent internet access increases exposure to prompt injection
from untrusted web content and to code or secret exfiltration.

### Position summary

| Question | sandbox-runtime | Codex CLI | Codex cloud | Egress |
| --- | --- | --- | --- | --- |
| Mediation optional once `MEDIATED` is selected | no | yes | no | no |
| Allowlist unit | domain | domain | domain | host+port+addr |
| Allowlist scope | config | config | environment | execution |
| Listener authenticated | no | no | n/a | yes |
| Method restriction | no | no | yes | planned |
| Phase-dependent network | no | no | yes | planned |
| Interception | opt-in | n/a | n/a | refused |

One factual difference is worth recording on its own: an exact-SNI guard that
verifies destination identity **without** TLS termination is not part of either
reference — sandbox-runtime's identity assurance beyond the allowlist comes
from optional MITM, and Codex enforces at the allowlist and method layers. This
design takes the non-terminating path described in §3.

## 8. Implementation phases

Each phase has one repository owner and one independently checkable outcome.
Release numbering is the roadmap's call, not this design's.

**Phase A — Egress `AF_UNIX` listener (Egress; D2; delivered in 0.5.0).** The
filesystem-path endpoint is separate from the numeric TCP setter. It rejects
overlong paths, embedded NULs, symlinks, pre-existing nodes and an unsafe or
changed parent. It requires a caller-created private directory, a mandatory
proxy credential and an explicit peer decision; same-EUID verification is
available as an additional check, never as a replacement for proxy
authentication. On destruction it unlinks only the socket identity the server
created. *Gate delivered:* HTTP, SOCKS, authentication and receipts run over
the Unix transport, with unsafe-path, changed-permission and replacement-safe
teardown tests.

**Phase B — bubblewrap bridge (Executor; D3, D4; delivered in Executor
0.13.0).** Ship an Executor-owned
relay/bootstrap binary linked to maelys-system. Extend the bubblewrap artifact
to reserve and mount the private socket path after user mounts, start the helper
inside the new namespace, wait for readiness, then launch the workload. Keep
`--unshare-net`; do not add a host veth or shared network namespace. *Gate:* an
ordinary unmodified HTTP and SOCKS client succeeds through the relay, direct
external and neighbouring-loopback connects fail, user mounts cannot shadow
the bridge, and every pre-ready failure prevents workload exec.

**Phase C — common connector and fd 4 frontend (Egress plus adapter; D13;
Egress half delivered on main).**
Factor exact admission, pinned connect, guarded relay and receipt production
behind an opaque Egress connector/session seam shared by the server and the fd 4
adapter. Preserve fd 4 request framing. Return the client end of a Egress-held
relay stream so the SNI guard and receipts remain truthful after connect.
*Gate:* the same destination matrix and policy digest produce the same
admission result through HTTP, SOCKS and fd 4; guarded fd 4 traffic cannot
bypass SNI; byte/close receipts are observed rather than guessed.

**Phase D — per-execution binding (optional Executor↔Egress adapter; D1, D5,
D8–D10).** Derive and seal the Egress policy from the resolved plan, create the
logical binding and credential, install fd 4, apply the profile environment,
compile the backend-specific reachability mechanism, enforce ready-before-exec
and destroy everything with the execution. Add the exact Seatbelt loopback
rule and behavioural probe here. *Gate:* concurrent executions with disjoint
allowlists cannot use each other's endpoint or credential; a leaked credential
is useless after teardown; no supported backend can fall back to ambient
network.

**Phase E — method restriction (Egress; D11).** Add an explicit permitted-method
set per destination, include it in the canonical digest input, enforce it
before forward-request rewriting and record the admitted method. Document it
as a plaintext forward-mode risk reducer. *Gate:* a denied method emits
`ERR_DENIED` and a receipt, while tests demonstrate that CONNECT and URL/header
exfiltration remain outside the guarantee.

**Phase F — setup/run profiles (deferred design; D12).** Do not implement from
the present text. First define the higher-level phase boundary, connection
draining, credential rotation, non-overlap and failure semantics, then decide
whether the seam belongs to Executor or Orchestrator.

**Phase G — ECH split for controlled origins (deferred design; D14).** Do not
implement from the present text. First specify an RFC 9849 client-facing
service contract: ECHConfig publication, key generation/storage/rotation,
HPKE and ClientHelloInner parsing, backend handoff including retry and
acceptance confirmation, bounded replay and denial-of-service handling, and a
receipt proving which inner name was admitted without claiming application
content visibility. The TLS/ECH provider must be an explicit new seam; the
existing listener-TLS provider is not widened implicitly. The scope excludes
arbitrary third-party destinations whose ECH key and backend are not under the
deployment's control. *Gate:* an interoperable controlled-origin test proves
that the backend completes end-to-end TLS, Egress admits the reconstructed inner
SNI without gaining application plaintext, and a non-cooperating origin is
refused rather than downgraded.

Phases A and B unblock standard clients under bubblewrap. Phase C unifies the
Maelys-aware fd 4 path. Phase D composes them per execution. Phase E is
independent. Phases F and G deliberately remain behind separate reviews.

## 9. Decisions

| # | Decision | State |
| --- | --- | --- |
| D1 | Seatbelt: host loopback TCP, one exact allowed endpoint | shipped with Executor 0.13.0 |
| D2 | bubblewrap: keep `--unshare-net`, cross through `AF_UNIX` | shipped with Executor 0.13.0 |
| D3 | In-namespace relay for tools needing a host:port | shipped with Executor 0.13.0 |
| D4 | Executor owns the relay/bootstrap; maelys-system supplies mechanics | shipped with Executor 0.13.0 |
| D5 | A `MEDIATED` plan never degrades to ambient network | standing |
| D6 | Exact cleartext SNI before any byte forwards | shipped |
| D7 | No TLS interception | standing |
| D8 | One immutable logical Egress binding and credential per execution | shipped for proxy frontend; fd-4 adapter pending |
| D9 | Explicit binding record joins decision/plan/policy digests | partial |
| D10 | No shared mutable policy/credential namespace in v1 | standing |
| D11 | Forward-method restriction is a limited risk reducer | to build |
| D12 | Setup/run profiles need a separate phase-switch design | deferred |
| D13 | fd 4 and HTTP/SOCKS share one Egress enforcement core | Egress connector shipped on main; fd-4 adapter pending |
| D14 | ECH split is a controlled-origin client-facing mode distinct from application-TLS termination | deferred design |

D9 is partial because the bottom link already ships:
`maelys_egress_receipt_policy_digest_hex` and `maelys_egress_receipt_invocation_id`
are on every receipt. The join to the plan and decision digests is the
Executor's remaining work.

## 10. Open questions

Deliberately unresolved; they should not be closed by implementation without a
decision.

1. **Unix mount shape.** Whether supported bubblewrap versions bind-mount the
   socket node itself or the private parent directory read-only. Behavioural
   tests, not pathname intuition, decide this.
2. **fd 4 returned stream kind.** Resolved for the current contract: Egress
   returns a host-loopback TCP client fd so Executor can preserve its published
   connected-TCP promise. A future opaque-stream ABI may permit `socketpair`,
   but does not change this release.
3. **Default method set.** Whether `GET`/`HEAD` is the default for every
   destination of an untrusted execution or only where the operator asks, given
   that it silently breaks any plaintext API the workload legitimately writes
   to.
4. **Instance density.** At what measured concurrency a shared host process is
   worth designing without weakening the per-execution logical binding.
5. **ECH split boundary.** Whether Egress should ever act as an RFC 9849
   client-facing server for controlled origins, and what separate provider,
   backend handoff, key-custody and receipt contract would make that promise
   precise. This is not a general solution for third-party egress.
