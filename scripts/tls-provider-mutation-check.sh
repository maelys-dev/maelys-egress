#!/bin/sh
# Prove that the real provider tests catch the original Unix receive bypass,
# independently for each TLS stack. Compile errors/timeouts are never kills.
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
temp_base=${TMPDIR:-/tmp}
work=$(mktemp -d "${temp_base%/}/maelys-egress-tls-mutations.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

# A passing unmodified control prevents an already broken test or fixture
# from masquerading as evidence that either mutation is detected.
if ! make -C "$root" tls-providers-check >"$work/control.log" 2>&1; then
    cat "$work/control.log" >&2
    exit 1
fi
printf '%s\n' 'TLS mutation control: both unmodified providers passed'

run_mutant() {
    provider=$1 old=$2 new=$3
    mutant="$work/$provider"
    mkdir -p "$mutant"
    (cd "$root" && tar --exclude=.git --exclude=build --exclude=dist -cf - .) |
        (cd "$mutant" && tar -xf -)
    python3 - "$mutant/providers/tls_$provider.c" "$old" "$new" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
source = path.read_text()
old, new = sys.argv[2:]
if source.count(old) != 1:
    raise SystemExit(f"expected exactly one provider receive anchor: {old!r}")
path.write_text(source.replace(old, new, 1))
PY
    binary="$mutant/build/bin/test-tls-$provider"
    if ! make -C "$mutant" BUILD="$mutant/build" "$binary" >"$work/$provider-build.log" 2>&1; then
        tail -30 "$work/$provider-build.log" >&2
        printf 'invalid build\n' >"$work/$provider.result"
        return
    fi
    status=0
    MAELYS_TLS_TEST_CERT="$mutant/build/tls-fixtures/tls-cert.pem" \
    MAELYS_TLS_TEST_KEY="$mutant/build/tls-fixtures/tls-key.pem" \
        perl -e 'alarm shift; exec @ARGV' 60 "$binary" >"$work/$provider-run.log" 2>&1 || status=$?
    if test "$status" -eq 1 && grep -q '^FAIL tests/test_tls_provider.c:' "$work/$provider-run.log"; then
        printf 'killed\n' >"$work/$provider.result"
    else
        cat "$work/$provider-run.log" >&2
        printf 'survived or invalid (exit %s)\n' "$status" >"$work/$provider.result"
    fi
}

run_mutant mbedtls \
    'egress_tls_socket_receive(&session->socket, buffer, bounded)' \
    'recv(session->socket.fd, buffer, bounded, 0)' &
run_mutant wolfssl \
    'egress_tls_socket_receive(&session->socket, buffer, (size_t)length)' \
    'recv(session->socket.fd, buffer, (size_t)length, 0)' &
wait

failed=0
for provider in mbedtls wolfssl; do
    result=$(cat "$work/$provider.result" 2>/dev/null || printf 'missing')
    printf 'TLS mutation %s: %s raw Unix recv\n' "$result" "$provider"
    if test "$result" = killed; then
        grep -m 1 '^FAIL tests/test_tls_provider.c:' "$work/$provider-run.log"
    else
        failed=1
    fi
done
test "$failed" -eq 0
printf '%s\n' 'TLS provider mutation check: 2/2 killed'
