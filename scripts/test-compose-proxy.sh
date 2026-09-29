#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
compose_file=$root/examples/compose-proxy/compose.yaml
image=${MAELYS_EGRESS_IMAGE:-maelys-egress-sidecar:compose-test}

case $image in
    *[!A-Za-z0-9_./:@-]*|'')
        echo "MAELYS_EGRESS_IMAGE contains unsupported characters" >&2
        exit 64
        ;;
esac

if ! docker image inspect "$image" >/dev/null 2>&1; then
    dependencies=${MAELYS_DEPENDENCIES_DIR:?set MAELYS_DEPENDENCIES_DIR or prebuild MAELYS_EGRESS_IMAGE}
    docker build \
        --build-context "maelys-system=$dependencies/maelys-system" \
        --build-context "maelys-cli=$dependencies/maelys-cli" \
        --build-context "agent-cli-spec=$dependencies/agent-cli-spec" \
        -f "$root/docker/Dockerfile.sidecar" -t "$image" "$root"
fi

export MAELYS_EGRESS_IMAGE="$image"
compose() {
    docker compose -f "$compose_file" "$@"
}
cleanup() {
    compose down --volumes --remove-orphans >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM

compose config --quiet
compose up --build -d

app_id=$(compose ps -aq app)
bridge_id=$(compose ps -aq proxy-bridge)
test -n "$app_id" && test -n "$bridge_id"
app_status=$(docker wait "$app_id")
compose logs --no-color
test "$app_status" -eq 0

test "$(docker inspect -f '{{.HostConfig.NetworkMode}}' "$app_id")" = none
bridge_network=$(docker inspect -f '{{.HostConfig.NetworkMode}}' "$bridge_id")
test "$bridge_network" = "container:$app_id" || {
    echo "proxy-bridge does not share the app network namespace" >&2
    exit 1
}

echo "compose-proxy-check: networkless application reached its allowed destination"
