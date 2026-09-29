# Docker Compose: proxy compatibility mode

This example runs an ordinary proxy-aware application without an ambient IP
network. Egress is the only service with an outbound network. A small bridge
shares the application's otherwise empty network namespace, listens on its
loopback interface and relays bytes to Egress's authenticated filesystem Unix
socket.

```text
app (network_mode: none)
  | HTTPS_PROXY=http://maelys:TOKEN@127.0.0.1:3128
  v
proxy-bridge (the app's network namespace)
  | shared AF_UNIX socket
  v
egress (outbound Docker network) --> github.com:443
```

Compose assigns Egress and the bridge the same deployment UID, independently
of the runtime image's built-in user. This lets the socket keep its private
`0600` mode and makes `unix_peer = same-euid` an additional check.
Authentication remains mandatory: the initializer generates a random token at
deployment time and writes two owner-only copies, one for Egress and one for
the separately identified application. No credential is stored in this
repository or printed in the logs. The initializer also copies the
configuration to an Egress-owned `0600` inode; a direct bind mount would keep
the host user's numeric UID on Linux and fail Egress's trusted-file rule.

## Run it

From the repository root, materialize the pinned build dependencies and build
the sidecar image:

```sh
dependency_root=$(mktemp -d)
sh scripts/checkout-dependencies.sh "$dependency_root"
docker build \
  --build-context maelys-system="$dependency_root/maelys-system" \
  --build-context maelys-cli="$dependency_root/maelys-cli" \
  --build-context agent-cli-spec="$dependency_root/agent-cli-spec" \
  -f docker/Dockerfile.sidecar \
  -t maelys-egress-sidecar:local .
```

Start the example and wait for the `app` container to report success:

```sh
docker compose -f examples/compose-proxy/compose.yaml up --build -d
docker compose -f examples/compose-proxy/compose.yaml logs -f app
```

Remove the containers and the volumes holding the ephemeral credentials and
socket:

```sh
docker compose -f examples/compose-proxy/compose.yaml down --volumes
rm -rf "$dependency_root"
```

Set `MAELYS_EGRESS_IMAGE` if the Egress image has another local name. Change
`TARGET_URL` and the exact allowlist together when adapting the example. The
application image may be replaced by any program that supports an authenticated
HTTP proxy; it needs the token copy and the proxy environment, but it does not
need the Unix socket, the Egress binary or an Egress library.

## Security boundary

`network_mode: none` is the enforcement in this example: the application and
bridge retain only their shared loopback interface. The bridge has the Egress
socket but no IP route; Egress has the IP route but does not share the
application's network namespace. Only Egress receives both sides needed to
reach an allowed destination.

This is the proxy compatibility mode, not the native mediated-connection
channel. It does not pass file descriptor 4, and the application holds a proxy
credential. A separate bootstrap protocol is still required for an Egress-aware
application in another container to acquire a native channel by pathname.
