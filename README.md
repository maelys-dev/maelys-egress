# Maelys Egress

Maelys Egress is a small, policy-enforced forward proxy for sandboxed workloads.
It accepts HTTP `CONNECT`, single-exchange HTTP/1.1 forward requests and SOCKS5,
then connects only to exact TCP destinations in a sealed allowlist.
The `channel broker` command also serves native Egress-aware
clients through a private Unix socket, without a proxy listener or bearer token.

```sh
maelys-egress channel broker --config /etc/maelys-egress.conf
```

A minimal broker configuration is:

```ini
schema_version = 1
channel_listen_unix = /run/maelys-egress/agent.sock
channel_principal = agent-001
allow = github.com:443
```

The socket's parent must already exist, belong to the broker's effective UID
and have mode `0700`, or `2750` with the broker in its capability group. Use
one socket per principal. The application links `libmaelys_egress_client` and
opens it with `maelys_egress_client_channel_open`, keeps that handle alive,
then passes its borrowed channel descriptor to `maelys_egress_client_connect`.
It does not receive an upstream socket: the returned stream remains mediated
by Egress. This does not sandbox the application or remove its other network
access; the supervisor must enforce that separately. The
[bootstrap v1 contract](protocol/egress-channel-bootstrap-v1.md) is frozen;
the [native Compose example](examples/compose-channel/README.md) demonstrates
a separate application container with `network_mode: none`.
Configuration is described by `config describe` and the
[generated configuration reference](docs/generated/config-reference.md).

The project owns the `maelys-egress` product and ABI namespace. A release
contains one canonical binary and symbol family; it does not carry duplicate
compatibility names.

```text
sandboxed workload                 embedding host
        |                                |
        | HTTP proxy / CONNECT / SOCKS5  | native connector session
        +------------------------------- v
  maelys-egress ABI 3
  +-------------------+
  | bounded parsers   |-- authentication
  | exact policy      |-- pinned DNS addresses
  | relay state       |-- backpressure/timeouts
  | receipts          |-- policy digest
  +---------+---------+
            |
            v
       libmaelys-sys reactor
       poll / epoll / kqueue
            |
            v
     allowed upstream only
```

The dependency direction is one-way:

```text
maelys-egress            -> libmaelys-sys
maelys-egress CLI binary -> libmaelys_cli (agent-cli/v2 framework)
```

Egress core contains no policy language, sandbox backend, MCP implementation or
concrete TLS library. It consumes the pinned Maelys System ABI 1 reactor and
descriptor primitives. Only the command-line binary links the pinned Maelys
CLI framework, which provides its `agent-cli/v2` contract; the library never
includes it. Its public handles remain opaque and expose no System
type. Optional reference modules bind the TLS seam to Mbed TLS or wolfSSL.

## What Egress provides

- exact host/port TCP allowlists, canonicalized and SHA-256 identified together
  with their resolved, pinned address set;
- DNS resolution at policy sealing and address pinning for the policy lifetime;
- non-global addresses denied unless the destination opts in explicitly;
- plaintext loopback listeners and optional TLS-protected remote listeners,
  authenticated by HTTP Bearer/Basic or SOCKS5 username/password;
- filesystem `AF_UNIX` listeners beneath a private mode-0700 directory, with
  mandatory proxy authentication and optional same-EUID peer verification;
- an explicit development-only unauthenticated loopback mode;
- bounded 16 KiB proxy headers, bounded per-direction buffers, connection
  limits, handshake/connect deadlines and idle deadlines;
- strict HTTP authority/`Host` equality, proxy credential stripping, rejection
  of ambiguous `Content-Length` and `Transfer-Encoding`;
- level-triggered relay with half-close propagation and backpressure;
- immutable borrowed receipts containing outcome, byte counts and policy
  digest;
- a backend-neutral nonblocking TLS provider seam.
- an opt-in exact-SNI guard for TLS tunnels, with verification recorded in the
  receipt and policy identity;
- immutable DNS generation replacement through `maelys_egress_policy_reseal`;
- an adversarial relay and mutation gate in addition to parser fuzzing.
- independently authenticated execution principals and credential-free receipt
  correlation through canonical invocation IDs;
- a profile seam producing `HTTP_PROXY`, `HTTPS_PROXY`, `ALL_PROXY` and their
  lowercase forms for an Executor adapter, without linking Executor into core.
- readiness-driven TLS listeners, including remote authenticated listeners and
  optional mutual TLS, through interchangeable Mbed TLS and wolfSSL modules.
- atomic policy replacement with generation-stable active streams and SIGHUP
  reload from the strict standalone configuration;
- opt-in per-principal connection/byte quotas, a separate loopback health and
  Prometheus metrics listener, and immutable metrics snapshots for embedders;
- restart-verifying, append-only HMAC-SHA-256 receipt chains with synchronous
  durability, plus a generic optional asymmetric/HSM attestor seam. HMAC alone
  is shared-key integrity, not public-key attestation.
- an authenticated native connector/session API returning the client end of a
  private TCP relay, never the upstream socket, and the mediated-connection
  channel that carries it to another process, so embedders and confined
  workloads preserve SNI, quotas, byte accounting and receipts without
  speaking HTTP or SOCKS themselves.

The proxy is not itself a sandbox. Strong enforcement requires the workload to
have no direct ambient network path; otherwise it can ignore proxy variables.

## TLS position

HTTPS through `CONNECT` is end-to-end encrypted between the workload and the
origin. Egress does not terminate or MITM it, so the minimal binary needs
neither Mbed TLS nor wolfSSL.

`include/maelys/egress_tls.h` defines a provider seam with opaque sessions and the
states `COMPLETE`, `WANT_READ`, `WANT_WRITE`, `CLOSED` and `FAILED`. The
optional Mbed TLS and wolfSSL modules implement that same contract without
changing the Egress ABI. They protect incoming proxy listeners; they do not
inspect the end-to-end TLS carried through `CONNECT`.

## Install

```sh
brew install maelys-dev/tap/maelys-egress
```

The formula builds the released source with its pinned Maelys System and
Maelys CLI and installs the daemon, `libmaelys_egress.a`, `libmaelys_sys.a`,
the headers and pkg-config files. It conflicts with the `maelys-warden`
formula until the tap carries one shared `maelys-system` formula. Prebuilt
tarballs, Debian and RPM packages with provenance attestations are attached
to every [GitHub release](https://github.com/maelys-dev/maelys-egress/releases).

## Build

```sh
make check
make asan-ubsan
make tsan
make analyze
make fuzz-smoke
make install-check
make tls-providers-check tls-binaries # optional development packages required
```

The build uses the exact commits recorded under `dependencies/` for Maelys
System, Maelys CLI and the agent CLI specification. Materialize them under one
explicit root, outside this working tree, then export that root:

```sh
sh scripts/checkout-dependencies.sh /tmp/maelys-egress-dependencies
export MAELYS_DEPENDENCIES_DIR=/tmp/maelys-egress-dependencies
make check
```

The grouped checkout prevents an unrelated sibling working copy from entering
the build. Packaging may instead link an installed Maelys System:
`make install MAELYS_SYSTEM_PREFIX=/opt/homebrew/opt/libmaelys-sys` accepts an
installation with ABI 1 and at least the pinned version, then installs neither
`libmaelys_sys` nor its headers. Every installation also writes
`share/maelys/commands/egress.json`, the digest-bound manifest that lets the
Maelys dispatcher run the daemon as `maelys egress`.

## CLI

All operational settings live in one strict, versioned configuration file:

```sh
schema_version = 1
listen = 127.0.0.1:8080
token_file = /run/user/1000/maelys-egress.token
allow_tls_sni = github.com:443
allow_tls_sni = api.anthropic.com:443
```

For a namespace bridge or local sidecar, create the socket parent first and
keep it private:

```sh
install -d -m 0700 /run/maelys-egress
schema_version = 1
listen_unix = /run/maelys-egress/egress.sock
unix_peer = same-euid
token_file = /run/secrets/maelys-egress-token
allow_tls_sni = github.com:443
```

Validate without opening a listener, then run until SIGINT or SIGTERM:

```sh
maelys-egress config validate --config /etc/maelys-egress.conf
maelys-egress serve --config /etc/maelys-egress.conf
```

Deployment examples are provided as a [systemd unit](packaging/systemd/maelys-egress.service),
a [launchd service](packaging/launchd/com.maelys.egress.plist), a
[container sidecar Dockerfile](docker/Dockerfile.sidecar), and a
[Docker Compose deployment](examples/compose-proxy/README.md) in which an
ordinary proxy-aware application has `network_mode: none`. The separate
[native Compose example](examples/compose-channel/README.md) gives an
Egress-aware application a mediated channel through a read-only socket volume,
with a distinct UID, no ambient network and no secret. Its integration test
proves allowed access, direct-network failure and policy refusal.

### Native connector

The proxy listeners are one way in. The other is the native connector of the
library: an embedder creates an authenticated in-process connector with
`maelys_egress_server_connector_create`, admits one exact TCP destination per
`maelys_egress_connector_session_open`, and receives a blocking `CLOEXEC` TCP
stream. That stream is Egress's own client end of a private relay, never the
upstream socket, so the SNI guard, quotas, byte accounting, half-close and
receipts apply exactly as they do to `CONNECT` and SOCKS5. With
`maelys_egress_config_set_native_only` the server opens no port and no
pathname at all: the policy, relay and receipt engine run, and only connectors
reach them.

This is the mode for a workload that must hold neither a proxy address nor a
credential: its supervisor hands it an already-admitted stream, and it can ask
for another only through that supervisor. The contract is declared in
[`include/maelys/egress.h`](include/maelys/egress.h) and exercised by
[`examples/native_connector.c`](examples/native_connector.c).

Across a process boundary, the same connector serves the
[mediated-connection channel](protocol/egress-channel-v1.md):
`maelys_egress_channel_create` binds a datagram pair to the connector and
returns the client end for the supervisor to hand over, at the descriptor
number of its own convention; the confined process links
`libmaelys_egress_client` alone — nothing of the proxy, and of maelys-system
only its descriptor-passing object — and
asks for one destination per `maelys_egress_client_connect`, receiving the
stream as a passed descriptor. [`examples/channel_supervisor.c`](examples/channel_supervisor.c)
and [`examples/channel_client.c`](examples/channel_client.c) are the two sides;
maelys-warden hands the channel to the sandboxed process on file descriptor 4.

## Documentation map

- [CLI reference](docs/cli.md) and
  [machine-readable CLI contract](docs/cli-contract.json): generated from the
  executable catalog;
- [configuration-key reference](docs/generated/config-reference.md): generated
  from the standalone configuration catalog;
- [Maelys CLI integration guide](docs/maelys-cli-guide.md): framework and
  dispatcher conventions used by the command;
- [Python and Node.js process SDKs](sdk/README.md): lifecycle automation and
  the responsibilities left to the application's proxy client;
- [mediated-connection channel](protocol/egress-channel-v1.md): the
  interprocess contract of the native connector, version 1;
- [C examples](examples/README.md): embedding, native connector, channel,
  reload, metrics, durable audit and attestation examples;
- [security policy](SECURITY.md), [licensing](LICENSING.md) and
  [release procedure](RELEASING.md).

Compileable consumers live in `examples/`. Pure Python and Node.js helpers in
`sdk/` manage the standalone process without changing the C ABI or inventing a
second proxy protocol.

```python
from maelys_egress import Destination, EgressConfig, EgressProcess

config = EgressConfig([Destination("github.com", 443, require_tls_sni=True)])
with EgressProcess(config) as egress:
    print(egress.proxy_url)
    print(egress.health())
```

```js
import { Destination, EgressConfig, EgressProcess } from "@maelys/egress";

const egress = new EgressProcess(new EgressConfig({
  destinations: [new Destination("github.com", 443, { requireTlsSni: true })],
}));
await egress.start();
console.log(egress.proxyUrl, await egress.health());
await egress.close();
```

The SDK source packages ship in the release; they are not yet published to
PyPI or npm. They are process-level helpers rather than native bindings.

Use `allow_tls_sni = github.com:443` when the destination identity must also
be enforced at the TLS ClientHello layer. Clients using ECH or no SNI are
refused by that policy rather than silently degraded.

The first stdout event is:

```json
{"schemaVersion":1,"contract":"maelys-egress-lifecycle/1","event":"ready","proxy":{"transport":"tcp","host":"127.0.0.1","port":8080},"admin":null,"policy":{"generation":1,"algorithm":"sha256","digest":"..."}}
```

Receipts and reload state follow as JSON Lines on stdout. Diagnostics remain on
stderr. Consumers must drain stdout until exit; the token is never printed.

For local tests only, set this in the configuration instead of `token_file`:

```text
unauthenticated_loopback = true
allow_private = 127.0.0.1:8080
```

For a TLS listener, build either optional binary and provide a PEM identity:

```sh
make tls-binaries
maelys-egress-mbedtls serve --config /etc/maelys-egress-tls.conf
```

Add `tls_ca` and `require_client_cert = true` for mutual TLS. The wolfSSL
binary accepts the identical configuration. The source distribution does not bundle
either TLS library and the normal `maelys-egress` binary remains dependency-free.
`make install-tls-modules` installs both optional archives, their constructor
header and the two provider-specific binaries; it is intentionally separate
from the minimal package install.

## Supported platforms

- Linux amd64/arm64 (`epoll` through Maelys System);
- macOS 15+ on Apple Silicon (`kqueue` through Maelys System).

Windows, UDP and transparent interception are outside the current contract. TLS
inspection is not part of the core or default mode; it requires a separate,
explicitly authorized PKI and threat-model design.

## License

MPL-2.0; see [LICENSING.md](LICENSING.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
