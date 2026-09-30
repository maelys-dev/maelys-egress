#!/bin/sh
# Mutation gate: each mutant is an independent copy of the tree with one
# security-relevant comparison inverted; the test suite must fail on every
# one. Mutants build and run in parallel, so the gate costs about one build
# and test cycle instead of five.
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
temp_base=$(printenv TMPDIR || printf '%s' /tmp)
temp_base=${temp_base%/}
work=$(mktemp -d "$temp_base/maelys-egress-mutations.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
system_dir=${MAELYS_SYSTEM_DIR:-${MAELYS_DEPENDENCIES_DIR:?set it or MAELYS_SYSTEM_DIR}/maelys-system}
cli_dir=${MAELYS_CLI_DIR:-${MAELYS_DEPENDENCIES_DIR:?set it or MAELYS_CLI_DIR}/maelys-cli}

# One sequential dependency build, shared read-only by every mutant, so the
# parallel mutants never race on the same dependency output directory.
deps="$work/deps"
mkdir -p "$deps"
make -C "$root" MAELYS_SYSTEM_DIR="$system_dir" MAELYS_CLI_DIR="$cli_dir" \
    BUILD="$deps" "$deps/deps/maelys-system/lib/libmaelys_sys.a" \
    "$deps/deps/maelys-cli/lib/libmaelys_cli.a" >/dev/null

run_mutant() {
    name=$1 file=$2 old=$3 new=$4
    mutant="$work/$name"
    mkdir -p "$mutant"
    (cd "$root" && tar --exclude=.git --exclude=build --exclude=dist -cf - .) |
        (cd "$mutant" && tar -xf -)
    python3 - "$mutant/$file" "$old" "$new" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
source = path.read_text()
old, new = sys.argv[2], sys.argv[3]
if source.count(old) != 1:
    raise SystemExit(f"mutation anchor count {source.count(old)} for {old!r}")
path.write_text(source.replace(old, new, 1))
PY
    if perl -e 'alarm shift; exec @ARGV' 60 make -C "$mutant" \
        MAELYS_SYSTEM_DIR="$system_dir" MAELYS_CLI_DIR="$cli_dir" \
        MAELYS_SYSTEM_BUILD="$deps/deps/maelys-system" \
        MAELYS_CLI_BUILD="$deps/deps/maelys-cli" \
        BUILD=build/mutant test >"$work/$name.log" 2>&1; then
        printf 'survived\n' >"$work/$name.result"
    else
        case "$name" in
        tls-*)
            # A compile failure or an unrelated suite timeout is not proof
            # that the TLS regression test detected this transport mutant.
            if grep -q '^FAIL tests/test_tls_socket' "$work/$name.log"; then
                printf 'killed\n' >"$work/$name.result"
            else
                printf 'invalid (no TLS assertion)\n' >"$work/$name.result"
                tail -20 "$work/$name.log" >&2
            fi ;;
        *) printf 'killed\n' >"$work/$name.result" ;;
        esac
    fi
}

run_mutant sni-host-mismatch src/core/clienthello.c \
    'strcmp(canonical, expected_host) != 0' \
    'strcmp(canonical, expected_host) == 0' &
run_mutant authority-mismatch src/core/http.c \
    'strcmp(header_host, out_request->host) != 0' \
    'strcmp(header_host, out_request->host) == 0' &
run_mutant credential-compare src/core/common.c \
    'return difference == 0u;' 'return difference != 0u;' &
run_mutant destination-port src/core/policy.c \
    'comparison == 0 && port == destination->port' \
    'comparison == 0 && port != destination->port' &
run_mutant relay-half-close src/server/relay.c \
    'maelys_sys_socket_shutdown(connection->upstream_socket, SHUT_WR)' \
    'maelys_sys_socket_shutdown(connection->upstream_socket, SHUT_RD)' &
run_mutant channel-host-bound src/core/channel.c \
    'out->host_length > MAELYS_EGRESS_CHANNEL_MAX_HOST' \
    'out->host_length >= MAELYS_EGRESS_CHANNEL_MAX_HOST' &
run_mutant channel-request-rights src/channel_server.c \
    '                 MAELYS_SYS_FDPASS_SURPLUS)) {' '                 0u)) {' &
run_mutant channel-status-denied src/channel_server.c \
    'case MAELYS_EGRESS_ERR_DENIED: return MAELYS_EGRESS_CHANNEL_DENIED;' \
    'case MAELYS_EGRESS_ERR_DENIED: return MAELYS_EGRESS_CHANNEL_OK;' &
run_mutant tls-ignore-control providers/socket_io.c \
    'if (flags != 0 || received == 0)' 'if (received == 0)' &
run_mutant tls-resume-poisoned providers/socket_io.c \
    'if (socket->failed) { errno = EPROTO; return -1; }' \
    'socket->failed = 0;' &
run_mutant tls-raw-unix-read providers/socket_io.c \
    'if (!socket->unix_stream)' 'if (socket->fd >= 0)' &
run_mutant tls-stream-family-restriction providers/socket_io.c \
    'getsockname(fd, (struct sockaddr *)&address, &address_length) != 0)' \
    'getsockname(fd, (struct sockaddr *)&address, &address_length) != 0 || (address.ss_family != AF_UNIX && address.ss_family != AF_INET && address.ss_family != AF_INET6))' &
wait

killed=0
total=0
for name in sni-host-mismatch authority-mismatch credential-compare \
    destination-port relay-half-close channel-host-bound channel-request-rights \
    channel-status-denied tls-ignore-control tls-resume-poisoned tls-raw-unix-read \
    tls-stream-family-restriction; do
    total=$((total + 1))
    result=$(cat "$work/$name.result" 2>/dev/null || printf 'missing')
    if test "$result" = killed; then
        killed=$((killed + 1))
        printf '%s\n' "mutation killed: $name"
    else
        printf '%s\n' "mutation $result: $name" >&2
    fi
done
test "$killed" -eq "$total" || {
    printf '%s\n' "mutation check: $killed/$total killed" >&2
    exit 1
}
printf '%s\n' "mutation check: $killed/$total killed"
