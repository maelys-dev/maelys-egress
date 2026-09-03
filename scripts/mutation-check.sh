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
system_dir=${MAELYS_SYSTEM_DIR:-$root/../maelys-system}
cli_dir=${MAELYS_CLI_DIR:-$root/../maelys-cli}

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
        printf 'killed\n' >"$work/$name.result"
    fi
}

run_mutant sni-host-mismatch src/clienthello.c \
    'strcmp(canonical, expected_host) != 0' \
    'strcmp(canonical, expected_host) == 0' &
run_mutant authority-mismatch src/http.c \
    'strcmp(header_host, out_request->host) != 0' \
    'strcmp(header_host, out_request->host) == 0' &
run_mutant credential-compare src/common.c \
    'return difference == 0u;' 'return difference != 0u;' &
run_mutant destination-port src/policy.c \
    'comparison == 0 && port == destination->port' \
    'comparison == 0 && port != destination->port' &
run_mutant relay-half-close src/server_relay.c \
    'maelys_sys_socket_shutdown(connection->upstream_socket, SHUT_WR)' \
    'maelys_sys_socket_shutdown(connection->upstream_socket, SHUT_RD)' &
wait

killed=0
total=0
for name in sni-host-mismatch authority-mismatch credential-compare \
    destination-port relay-half-close; do
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
