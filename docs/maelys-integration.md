# Maelys Sandbox–Executor–Egress integration

## Start with one concrete requirement

Suppose an agent may read its workspace, write only its build directory and
connect only to `github.com:443`:

```text
filesystem: READ workspace, WRITE workspace/build
network:    MEDIATED TCP github.com:443
```

No single Maelys component implements that whole sentence. The separation is
intentional:

```text
resolved decision       portable compilation       launch/enforcement

canonical MIR ─────────► Maelys Sandbox Policy ───► Maelys Warden
                                                        │
                                  OS confinement ◄──────┤
                                                        │
                                  network mediation ◄───┴──► maelys-egress
```

## What each component actually does

### 1. MIR records the decision

Canonical MIR is a stable binary statement of permissions already decided. It
can be produced by Maelys Datalog, a JSON compiler or another trusted policy
frontend. It contains abstract roots and exact network decisions, not command
lines, host paths, proxy ports or credentials.

```text
NETWORK_MEDIATED
ALLOW TCP github.com 443
```

The MIR digest identifies that portable decision.

### 2. Maelys Sandbox Policy compiles; it does not launch

Sandbox combines MIR with trusted host context:

- `WORKSPACE` becomes a concrete host path;
- filesystem rules become an immutable `maelys_sandbox_policy_plan_t`;
- `NETWORK_MEDIATED` keeps the exact TCP destinations;
- a host-chosen mediator identifier says which integration is allowed;
- requested primitives are checked against prospective backend capabilities.

Sandbox itself does not call Bubblewrap, compile a Seatbelt profile, start
Egress or spawn the workload. Think of it as a portable plan compiler, not the OS
sandbox process.

### 3. Maelys Warden launches and supervises

The Sandbox-to-Executor adapter converts the immutable Sandbox plan into
`maelys_executor_plan_t`. Executor then:

- selects a backend whose probed capabilities cover the complete plan;
- prepares the backend-specific artifact;
- spawns, waits, stops and releases the process;
- records backend identity, plan digest, outcome and timestamps.

There is no “try confinement, then silently use POSIX” path. If the selected
backend cannot enforce one requested primitive, launch is refused.

### 4. maelys-egress enforces the network destination

Egress receives the exact mediated destinations through an integration adapter.
It seals DNS addresses, authenticates the execution principal, parses
HTTP/SOCKS, applies host/port and optional SNI checks, relays bytes, applies
quotas and emits connection receipts.

Egress does not restrict filesystem access or own process lifecycle. Sandbox does
not resolve DNS. Executor must not independently maintain a broader second
network allowlist. The adapter copies the MIR destinations into one Egress policy
so there is one decision and one network enforcement engine.

## Backend differences

The backend is the mechanism Executor uses to apply the plan to one process.

| Backend | Platform | What it can enforce | Egress integration consequence |
|---|---|---|---|
| POSIX | Linux/macOS | Process launch and basic process-group supervision only | Cannot remove ambient network or enforce filesystem confinement. A mediated/confined plan must be refused; suitable only for explicitly trusted-local work. |
| Bubblewrap | Linux amd64/arm64 | User/PID/network namespaces, deny-default filesystem view, mappings/tmpfs, filesystem rules, seccomp-assisted network denial | Best fit for standard tools: keep a private network namespace and expose only a mounted AF_UNIX Egress socket through a loopback relay. |
| Seatbelt | macOS 15+, Apple Silicon | SBPL filesystem and network policy inherited by descendants | No Linux-style mount namespace, view mapping or tmpfs. A Maelys-aware inherited-fd frontend is natural; a standard proxy frontend requires a narrowly permitted local proxy route and must be supplied/proven by the Executor adapter. |
| OCI/Firecracker | Future/optional Executor backends | Container or VM isolation | Egress can remain a host service or sidecar, but the backend must expose only the authenticated mediator route and no ambient egress. |

POSIX is therefore not a weaker sandbox backend; it is a non-confining launcher.
Bubblewrap and Seatbelt are not Egress plugins. They are Executor backends that
create the confinement boundary in which Egress becomes non-bypassable.

## Standard applications under Bubblewrap

An ordinary program understands HTTP/SOCKS proxy settings but knows nothing
about Maelys. The topology is:

```text
host
┌──────────────────────────────────────────────────────────────┐
│ Egress AF_UNIX listener                                        │
│ /run/maelys-egress/execution-42.sock                            │
│        ▲ authentication + one sealed destination policy      │
└────────┼──────────────────────────────────────────────────────┘
         │ only this socket file is mounted across the boundary
┌────────┼──────── Bubblewrap private network namespace ────────┐
│ protocol-blind relay                                         │
│ AF_UNIX ◄──► 127.0.0.1:43119                                 │
│                    ▲                                         │
│ workload uses HTTP_PROXY / HTTPS_PROXY / ALL_PROXY            │
└───────────────────────────────────────────────────────────────┘
```

Step by step:

1. The adapter derives a Egress policy from the MIR destinations and binds one
   execution principal to the private listener.
2. Egress opens a private AF_UNIX listener on the host.
3. Executor mounts only that socket into Bubblewrap's otherwise isolated
   network/filesystem view.
4. A small relay inside the namespace exposes a conventional loopback proxy
   port and transports bytes to the Unix socket without making policy.
5. Executor injects standard proxy variables without URL userinfo into the
   workload environment.
6. Egress assigns every HTTP/SOCKS stream to the principal immutably bound to
   that listener and remains the sole component that resolves, quotas and
   authorizes destinations.

No credential crosses argv, environment, a VM kernel command line, console,
receipt or error path. The endpoint identity is the private pathname and the
device/inode-verified socket created for that execution. Same-EUID peer
credentials are defense in depth; they do not identify one execution among
several VMM or relay processes running under the same account.

A standard program that honors proxy settings works. A program that ignores
them or uses `curl --noproxy` has no ambient IP route and fails; it does not
bypass Egress. Sharing the host network namespace merely to make the proxy
reachable would break this guarantee and must be refused.

Egress supplies the authenticated AF_UNIX listener and HTTP/SOCKS engine.
Executor 0.13.0 supplies the Bubblewrap namespace relay/bootstrap and the
Seatbelt exact-loopback route. Their setup and lifecycle remain Executor
responsibilities rather than Egress backends.

## Maelys-aware clients and fd 4

A Maelys-aware SDK can use an inherited Unix datagram control channel on child
fd 4:

```text
workload --connect_tcp("github.com", 443)--> fd 4 broker
workload <--SCM_RIGHTS connected stream--------- broker
```

This avoids proxy environment variables and HTTP/SOCKS negotiation. It is
namespace-proof because an already-open Unix fd survives entry into a private
network namespace.

However, handing the raw upstream TCP socket to the workload would bypass
Egress's SNI guard, byte quota, half-close handling and terminal receipt. The
full-semantics design instead returns the client end of a private loopback TCP
pair while Egress retains and relays the accepted end:

```text
workload fd ── private TCP pair ── Egress relay ── guarded upstream TCP
```

Egress now exposes this as an opaque native connector/session API. Connector
creation authenticates one configured principal once. Each session request
then enters the same exact policy lookup, pinned connect, SNI guard, quota,
relay, half-close and receipt machinery as HTTP and SOCKS. The session fd is a
blocking CLOEXEC TCP socket whose peer remains owned by Egress; it is never the
upstream socket.

Egress deliberately does **not** parse fd 4. The remaining adapter work belongs
beside Executor: decode its canonical control request, call
`maelys_egress_connector_session_open()`, then pass the returned fd through
`SCM_RIGHTS`. Until that migration lands, Executor's current direct fd-4 broker
is not automatically equivalent to the Egress path.

## Seatbelt topology

Seatbelt has no Bubblewrap-style network namespace into which a loopback relay
can be placed. Two approaches are conceptually possible:

```text
Maelys-aware client: inherited fd 4 ──► Egress-relayed stream

standard client: HTTP/SOCKS ──► one explicitly permitted local proxy endpoint
                                all other network operations denied by SBPL
```

The first avoids a network exception but requires client SDK support and the
fd-4 adapter migration. Executor 0.13.0 implements the second route with one
exact loopback endpoint and fail-closed capability probing; it does not fall
back to unrestricted networking.

## TLS in this architecture

For a host-local AF_UNIX bridge, filesystem isolation plus proxy authentication
usually protects the client-to-Egress hop. If Egress is a remote service or traffic
crosses an untrusted host network, use the Mbed TLS or wolfSSL listener binary.
That listener TLS protects the proxy credential; origin HTTPS remains
end-to-end. Backend confinement requirements do not change with the TLS
provider.

## Joining the evidence

One execution may produce several Egress connection receipts. A useful binding
record contains:

```text
MIR digest M                    portable decision
Executor plan digest E          mechanical launch plan
Egress policy digest N            resolved destinations and DNS pins
Egress policy generation G        active network policy version
invocation_id                   correlation without exposing credentials
Executor terminal status        process outcome
Egress receipt(s)                 destinations, bytes and network outcomes
```

The adapter is the only layer that knows all three APIs, so it owns this
binding. Until an Executor adapter persists Egress receipt events with the
execution receipt, the two evidence streams can be correlated by invocation ID
but are not one composite attestation.

## Delivered versus integration work

| Capability | Current status |
|---|---|
| Standalone HTTP forward/CONNECT and SOCKS5 | delivered |
| Authenticated TCP and AF_UNIX listeners | delivered |
| DNS/SNI enforcement, quotas, receipts and durable audit | delivered |
| Bubblewrap AF_UNIX endpoint and namespace relay | delivered by Egress + Executor 0.13.0 |
| Seatbelt standard-proxy route | delivered by Egress + Executor 0.13.0 |
| Native connector entering the same Egress relay and receipt core | delivered in Egress main |
| fd 4 request translated to a native connector session | Executor-Egress adapter work |
| Composite Executor + Egress attestation | integration work |
