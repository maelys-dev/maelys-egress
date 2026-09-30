# Docker Compose: native channel, no application network or secret

This experimental example runs an Egress-aware C application in a separate
container with `network_mode: none`. It links only `libmaelys_egress_client`,
opens a Unix bootstrap socket, keeps the returned handle alive while making
channel requests, and uses the mediated stream descriptor. There is no HTTP
proxy bridge, proxy environment, bearer token, Warden dependency or assigned
descriptor number. The bootstrap wire tables are not frozen yet.

```text
app (UID 10002, supplementary group 20000, network_mode: none)
  | read-only named volume: /run/maelys-egress/channel.sock
  | bootstrap -> lifetime lease + native channel descriptor
  v
egress (UID 10001, group 20000): channel broker
  | exact policy: upstream:8080 allowed; upstream:8081 denied
  v
upstream (local TCP fixture; both ports listen)
```

The two workloads are `app` and `egress`. An initializer sets ownership/modes
and copies the configuration to a trusted inode; a local fixture provides a
repeatable destination without Internet access. The fixture and broker share
an internal Docker network, with no host-published ports. The application has
only loopback networking (kernel-created tunnel devices, if present, must be
down and have no IP address). Docker Desktop runs these containers inside
its Linux VM; Egress need not be installed on the macOS host.

## Run the integration example

Docker Engine or Docker Desktop, Compose v2 with BuildKit, Python 3 and the
pinned source dependencies are required. From the repository root:

```sh
dependency_root=$(mktemp -d)
sh scripts/checkout-dependencies.sh "$dependency_root"
MAELYS_DEPENDENCIES_DIR="$dependency_root" make compose-channel-check
```

The gate builds this checkout, runs the example in a fresh randomly named
Compose project and removes only that project's containers, volumes and
image tags afterwards. Build caches and the dependency directory remain
available for reuse. It fails rather than skipping when Docker is unavailable.

For a persistent manual run, build the sidecar and export its SDK build stage:

```sh
build_egress_image() {
  docker build "$@" \
    --build-context maelys-system="$dependency_root/maelys-system" \
    --build-context maelys-cli="$dependency_root/maelys-cli" \
    --build-context agent-cli-spec="$dependency_root/agent-cli-spec" \
    -f docker/Dockerfile.sidecar .
}
build_egress_image --target build -t maelys-egress-build:local
build_egress_image -t maelys-egress-sidecar:local
docker build -f examples/compose-channel/Dockerfile.app \
  -t maelys-egress-native-app:local examples/compose-channel
docker compose -f examples/compose-channel/compose.yaml up --no-build -d
docker compose -f examples/compose-channel/compose.yaml logs -f app
```

The finite application must exit zero after printing six successful checks.
Remove this manual deployment when finished:

```sh
docker compose -f examples/compose-channel/compose.yaml down --volumes
```

## What the test establishes

- The running app container has exactly `NetworkMode=none`, a read-only root,
  no capabilities, `no-new-privileges`, a distinct UID and one read-only volume.
- The broker owns the socket parent (`10001:20000`, mode `2750`) and socket
  (`10001:20000`, mode `0660`). The application cannot write the directory.
- The destination answers over the native stream. It returns its own numeric
  IP, so the application probes that same live destination without DNS and
  requires `ENETUNREACH` or `EHOSTUNREACH`, not just any connection error.
- A channel request for the other listening port returns the protocol's
  `DENIED`, not a timeout or connection refusal.
- Closing the bootstrap handle releases the broker's single capacity slot.
  Reacquiring it proves cleanup completed before testing that an already
  returned stream remains usable; the application then half-closes it and
  reads EOF.
- Lifecycle JSON conforms to its schema; both receipts carry `compose-agent`
  and `native-compose`, the expected destinations, results and byte counts.
- The app image copies only its executable from the SDK build stage. Linking
  uses only the client archive, without the proxy core, CLI or `-pthread`;
  symbol checks reject accidental dependencies.

The required `docker` CI job runs this gate as well as the existing proxy
compatibility example. Other native CI jobs compile the example against the
standalone archive, including on macOS. The Compose execution itself uses a
Linux kernel; it does not replace the native macOS descriptor-passing tests.

## Adapt it to your program

The small [application](app.c) shows `maelys_egress_client_channel_open`, the
borrowed descriptor used with `maelys_egress_client_connect`, and the separate
lifetimes of the bootstrap handle and destination stream. Keep those ownership
rules when replacing its assertions/echo exchange with your own protocol.
Use your application's TLS stack on the returned descriptor for HTTPS; this
plain TCP fixture is not a TLS termination example.

The path is the capability: give each principal its own broker/socket and
private volume. Do not mount this volume into unrelated applications. The
shared group authorizes access across different UIDs; it is not a peer-identity
mapping. Never share a writable parent directory with the application.

For a real remote destination, update the exact allowlist and attach only
Egress to the required outbound network. Do not remove the application's
`network_mode: none`. The initializer and fixture are example infrastructure,
not new Egress commands. Docker supplies the isolation; Egress mediates the
streams granted through the channel and does not itself sandbox a process.

The deployment uses standard Compose features documented under
[services](https://docs.docker.com/reference/compose-file/services/) and
[networking](https://docs.docker.com/compose/how-tos/networking/).
