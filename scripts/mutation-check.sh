#!/bin/sh
# Mutation gate: each mutant is an independent copy of the tree with one
# security-relevant comparison inverted; the test suite must fail on every
# one. Four mutants at a time bound compiler contention on CI runners.
# Compilation has its own deadline: it must not consume the test budget or
# count as evidence that a regression assertion detected the mutation.
set -eu
scope=${1:-all}
case "$scope" in all|bootstrap) ;; *) echo "expected all or bootstrap" >&2; exit 1 ;; esac
build_target=test-build
test_target=test
if test "$scope" = bootstrap; then
    build_target=bootstrap-test-build
    test_target=bootstrap-test
fi

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
    if ! perl -e 'alarm shift; exec @ARGV' 300 make -C "$mutant" \
        MAELYS_SYSTEM_DIR="$system_dir" MAELYS_CLI_DIR="$cli_dir" \
        MAELYS_SYSTEM_BUILD="$deps/deps/maelys-system" \
        MAELYS_CLI_BUILD="$deps/deps/maelys-cli" \
        BUILD=build/mutant "$build_target" >"$work/$name-build.log" 2>&1; then
        printf 'invalid (build failed)\n' >"$work/$name.result"
        tail -20 "$work/$name-build.log" >&2
        return
    fi
    if perl -e 'alarm shift; exec @ARGV' 60 make -C "$mutant" \
        MAELYS_SYSTEM_DIR="$system_dir" MAELYS_CLI_DIR="$cli_dir" \
        MAELYS_SYSTEM_BUILD="$deps/deps/maelys-system" \
        MAELYS_CLI_BUILD="$deps/deps/maelys-cli" \
        BUILD=build/mutant "$test_target" >"$work/$name.log" 2>&1; then
        printf 'survived\n' >"$work/$name.result"
    else
        case "$name" in
        native-*)
            if grep -q '^FAIL tests/test_egress.c:' "$work/$name.log"; then
                printf 'killed\n' >"$work/$name.result"
            else
                printf 'invalid (no native identity assertion)\n' >"$work/$name.result"
                tail -20 "$work/$name.log" >&2
            fi ;;
        tls-*)
            # A compile failure or an unrelated suite timeout is not proof
            # that the TLS regression test detected this transport mutant.
            if grep -q '^FAIL tests/test_tls_socket' "$work/$name.log"; then
                printf 'killed\n' >"$work/$name.result"
            else
                printf 'invalid (no TLS assertion)\n' >"$work/$name.result"
                tail -20 "$work/$name.log" >&2
            fi ;;
        bootstrap-blocking-destruction)
            if grep -Eq '^FAIL (\./)?tests/test_broker_faults\.c:' "$work/$name.log"; then
                printf 'killed\n' >"$work/$name.result"
            else
                printf 'invalid (no broker progress assertion)\n' >"$work/$name.result"
                tail -20 "$work/$name.log" >&2
            fi ;;
        bootstrap-sliding-deadline|bootstrap-request-truncation|bootstrap-response-truncation)
            if grep -Eq '^FAIL (\./)?tests/test_bootstrap_gates\.c:' "$work/$name.log"; then
                printf 'killed\n' >"$work/$name.result"
                grep -E '^FAIL ' "$work/$name.log" | sed "s/^/$name: /"
            else
                printf 'invalid (no new bootstrap regression assertion)\n' >"$work/$name.result"
                tail -20 "$work/$name.log" >&2
            fi ;;
        bootstrap-*)
            if grep -Eq '^FAIL (\./)?tests/(test_(bootstrap_client|bootstrap_gates|operations|broker_faults)\.c|tls_socket_fixture\.h):' "$work/$name.log"; then
                printf 'killed\n' >"$work/$name.result"
            else
                printf 'invalid (no bootstrap assertion)\n' >"$work/$name.result"
                tail -20 "$work/$name.log" >&2
            fi ;;
        *) printf 'killed\n' >"$work/$name.result" ;;
        esac
    fi
}

queued=0
selected=''
schedule_mutant() {
    case "$scope:$1" in bootstrap:bootstrap-*|all:*) ;; *) return ;; esac
    selected="$selected $1"
    run_mutant "$@" &
    queued=$((queued + 1))
    if test "$queued" -eq 4; then
        wait
        queued=0
    fi
}

schedule_mutant sni-host-mismatch src/core/clienthello.c \
    'strcmp(canonical, expected_host) != 0' \
    'strcmp(canonical, expected_host) == 0'
schedule_mutant authority-mismatch src/core/http.c \
    'strcmp(header_host, out_request->host) != 0' \
    'strcmp(header_host, out_request->host) == 0'
schedule_mutant credential-compare src/core/common.c \
    'return difference == 0u;' 'return difference != 0u;'
schedule_mutant destination-port src/core/policy.c \
    'comparison == 0 && port == destination->port' \
    'comparison == 0 && port != destination->port'
schedule_mutant relay-half-close src/server/relay.c \
    'maelys_sys_socket_shutdown(connection->upstream_socket, SHUT_WR)' \
    'maelys_sys_socket_shutdown(connection->upstream_socket, SHUT_RD)'
schedule_mutant channel-host-bound src/core/channel.c \
    'out->host_length > MAELYS_EGRESS_CHANNEL_MAX_HOST' \
    'out->host_length >= MAELYS_EGRESS_CHANNEL_MAX_HOST'
schedule_mutant channel-request-rights src/channel_server.c \
    '                 MAELYS_SYS_FDPASS_SURPLUS)) {' '                 0u)) {'
schedule_mutant channel-status-denied src/channel_server.c \
    'case MAELYS_EGRESS_ERR_DENIED: return MAELYS_EGRESS_CHANNEL_DENIED;' \
    'case MAELYS_EGRESS_ERR_DENIED: return MAELYS_EGRESS_CHANNEL_OK;'
schedule_mutant tls-ignore-control providers/socket_io.c \
    'if (flags != 0 || received == 0)' 'if (received == 0)'
schedule_mutant tls-resume-poisoned providers/socket_io.c \
    'if (socket->failed) { errno = EPROTO; return -1; }' \
    'socket->failed = 0;'
schedule_mutant tls-raw-unix-read providers/socket_io.c \
    'if (!socket->unix_stream)' 'if (socket->fd >= 0)'
schedule_mutant tls-stream-family-restriction providers/socket_io.c \
    'getsockname(fd, (struct sockaddr *)&address, &address_length) != 0)' \
    'getsockname(fd, (struct sockaddr *)&address, &address_length) != 0 || (address.ss_family != AF_UNIX && address.ss_family != AF_INET && address.ss_family != AF_INET6))'
schedule_mutant bootstrap-identity-field src/core/bootstrap_codec.c \
    'memcmp(bytes, "MEBQ", 4u) || bytes[6] || bytes[7])' \
    'memcmp(bytes, "MEBQ", 4u))'
schedule_mutant bootstrap-descriptor-cardinality client/channel_open.c \
    'int malformed = flags || !received ||' \
    'int malformed = !received ||'
schedule_mutant bootstrap-capacity-release src/channel_broker.c \
    'memset(slot, 0, sizeof(*slot));' \
    'memset(slot, 0, sizeof(*slot)); slot->phase = BROKER_LEASE;'
schedule_mutant bootstrap-lease-destruction src/channel_broker.c \
    'if (!request) return 0; /* No post-request data or control, even zero-byte rights. */' \
    'if (!request) return 1; /* Mutant ignores lease violations. */'
schedule_mutant bootstrap-early-busy client/channel_open.c \
    'if (io == MAELYS_SYS_ERR_CLOSED || io == MAELYS_SYS_ERR_RESET) break;' \
    'if (io == MAELYS_SYS_ERR_CLOSED || io == MAELYS_SYS_ERR_RESET) goto fail;'
schedule_mutant bootstrap-blocking-destruction src/channel_broker.c \
    '    egress_channel_stop(slot->channel);' \
    '    maelys_egress_channel_destroy(slot->channel); reset_slot(slot); return;'
schedule_mutant bootstrap-sliding-deadline src/channel_broker.c \
    '            slot->progress += received;' \
    '            if (maelys_sys_deadline_after(broker->handshake_timeout, &slot->deadline) != MAELYS_SYS_OK) { return 0; } slot->progress += received;'
schedule_mutant bootstrap-request-truncation src/channel_broker.c \
    'if (flags || !received) return answer(broker, slot, EGRESS_BOOTSTRAP_MALFORMED);' \
    'if ((flags & ~(unsigned)MAELYS_SYS_FDPASS_CONTROL_TRUNCATED) || !received) return answer(broker, slot, EGRESS_BOOTSTRAP_MALFORMED);'
schedule_mutant bootstrap-response-truncation client/channel_open.c \
    'int malformed = flags || !received ||' \
    'int malformed = (flags & ~(unsigned)MAELYS_SYS_FDPASS_CONTROL_TRUNCATED) || !received ||'
schedule_mutant native-trusted-scope src/server/connector.c \
    'server->config.native_only && server->config.native_principal_bound &&' \
    'server->config.native_only &&'
schedule_mutant native-immutable-binding src/config.c \
    'config->native_principal_bound = 1;' 'config->native_principal_bound = 0;'
wait

killed=0
total=0
for name in $selected; do
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
