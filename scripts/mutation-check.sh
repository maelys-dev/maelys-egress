#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
temp_base=$(printenv TMPDIR || printf '%s' /tmp)
work=$(mktemp -d "$temp_base/maelys-egress-mutations.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

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
    if perl -e 'alarm shift; exec @ARGV' 35 make -C "$mutant" \
        MAELYS_SYSTEM_DIR="${MAELYS_SYSTEM_DIR:-$root/../maelys-system}" \
        MAELYS_CLI_DIR="${MAELYS_CLI_DIR:-$root/../maelys-cli}" BUILD=build/mutant test \
        >/dev/null 2>&1; then
        printf '%s\n' "mutation survived: $name" >&2
        return 1
    fi
    printf '%s\n' "mutation killed: $name"
}

run_mutant sni-host-mismatch src/clienthello.c \
    'strcmp(canonical, expected_host) != 0' \
    'strcmp(canonical, expected_host) == 0'
run_mutant authority-mismatch src/http.c \
    'strcmp(header_host, out_request->host) != 0' \
    'strcmp(header_host, out_request->host) == 0'
run_mutant credential-compare src/common.c \
    'return difference == 0u;' 'return difference != 0u;'
run_mutant destination-port src/policy.c \
    'comparison == 0 && port == destination->port' \
    'comparison == 0 && port != destination->port'
run_mutant relay-half-close src/server_relay.c \
    'shutdown(connection->upstream_fd, SHUT_WR)' \
    'shutdown(connection->upstream_fd, SHUT_RD)'

printf '%s\n' 'mutation check: 5/5 killed'
