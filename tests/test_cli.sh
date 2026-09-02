#!/bin/sh
set -eu

binary=${1:?maelys-egress binary required}
if test "$(uname -s)" = Darwin; then
    root=$(mktemp -d /private/tmp/maelys-egress-cli-XXXXXX)
else
    root=$(mktemp -d)
fi
daemon_pid=
cleanup() {
    if test -n "$daemon_pid" && kill -0 "$daemon_pid" 2>/dev/null; then
        kill -TERM "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -rf "$root"
}
trap cleanup EXIT HUP INT TERM
chmod 0700 "$root"

# Reads one dotted path from a JSON file; formatting-independent.
json_get() {
    python3 -c 'import json, sys
value = json.load(open(sys.argv[1]))
for key in sys.argv[2].split("."):
    value = value[int(key)] if key.isdigit() else value[key]
print(value)' "$1" "$2"
}

token=$root/token
config=$root/egress.conf
socket=$root/egress.sock
printf '%s\n' 0123456789abcdef0123456789abcdef >"$token"
chmod 0600 "$token"
cat >"$config" <<EOF
schema_version = 1
listen_unix = $socket
unix_peer = same-euid
token_file = $token
max_connections = 16
allow_private = 127.0.0.1:9
EOF
chmod 0600 "$config"

"$binary" config validate --config "$config" --format json --compact |
    grep -q '"contract":"agent-cli/v2".*"valid":true'

"$binary" describe serve --format json --compact |
    grep -q '"outputMode":"protocol-stream".*"protocol":"maelys-egress-lifecycle/1"'
"$binary" version | grep -q '^maelys-egress [0-9]'
"$binary" config describe --format json --compact |
    grep -q '"tlsListener":\(true\|false\)'
"$binary" config describe --format json --compact |
    grep -q '"name":"schema_version"'
"$binary" completion zsh | grep -q '^#compdef maelys-egress'

# An unreadable or untrusted file is an execution failure: exit 1, envelope
# on stderr, nothing on stdout.
cp "$config" "$root/unsafe.conf"
chmod 0666 "$root/unsafe.conf"
status=0
"$binary" config validate --config "$root/unsafe.conf" --format json \
    >"$root/out" 2>"$root/err" || status=$?
test "$status" -eq 1
test "$(json_get "$root/err" ok)" = False
test "$(json_get "$root/err" error.code)" = ACCESS_DENIED
test ! -s "$root/out"
status=0
"$binary" config validate --config "$root/missing.conf" --format json \
    >"$root/out" 2>"$root/err" || status=$?
test "$status" -eq 1
test "$(json_get "$root/err" error.code)" = NOT_FOUND

# Invalid content is a completed validation report: exit 2, data on stdout,
# nothing on stderr.
cp "$config" "$root/duplicate.conf"
printf 'max_connections = 17\n' >>"$root/duplicate.conf"
status=0
"$binary" config validate --config "$root/duplicate.conf" --format json --compact \
    >"$root/out" 2>"$root/err" || status=$?
test "$status" -eq 2
test "$(json_get "$root/out" ok)" = True
test "$(json_get "$root/out" exitCode)" = 2
test "$(json_get "$root/out" data.valid)" = False
test "$(json_get "$root/out" data.diagnostics.0.code)" = VALIDATION_FAILED
test ! -s "$root/err"
status=0
"$binary" config validate --config "$root/duplicate.conf" >"$root/out" 2>"$root/err" || status=$?
test "$status" -eq 2
grep -q '^Configuration is invalid: \[VALIDATION_FAILED\]' "$root/out"

if "$binary" --config "$config" >/dev/null 2>&1; then
    echo "removed option-only invocation was accepted" >&2
    exit 1
fi

"$binary" serve --config "$config" --non-interactive >"$root/ready" 2>"$root/diagnostics" &
daemon_pid=$!
attempt=0
while test ! -S "$socket" && test "$attempt" -lt 100; do
    sleep 0.05
    attempt=$((attempt + 1))
done
test -S "$socket"
grep -q '"event":"ready".*"transport":"unix"' "$root/ready"
kill -TERM "$daemon_pid"
wait "$daemon_pid"
daemon_pid=
test ! -e "$socket"

audit_key=$root/audit.key
audit_log=$root/audit.jsonl
printf '%s' 0123456789abcdef0123456789abcdef >"$audit_key"
chmod 0600 "$audit_key"
cat >"$config" <<EOF
schema_version = 1
listen = 127.0.0.1:0
admin_listen = 127.0.0.1:0
token_file = $token
max_connections = 16
quota_connections = 2
quota_bytes = 4096
quota_total_bytes = 8192
audit_log = $audit_log
audit_key_file = $audit_key
audit_key_id = cli-test
allow_private = 127.0.0.1:9
EOF
chmod 0600 "$config"
"$binary" serve --config "$config" >"$root/ops-ready" 2>"$root/ops-log" &
daemon_pid=$!
attempt=0
while ! grep -q '"event":"ready"' "$root/ops-ready" 2>/dev/null && test "$attempt" -lt 100; do
    sleep 0.05
    attempt=$((attempt + 1))
done
grep -q '"event":"ready"' "$root/ops-ready"
proxy_port=$(python3 -c 'import json,sys; print(json.loads(sys.stdin.readline())["proxy"]["port"])' <"$root/ops-ready")
admin_port=$(python3 -c 'import json,sys; print(json.loads(sys.stdin.readline())["admin"]["port"])' <"$root/ops-ready")
curl --fail --silent --show-error --max-time 2 \
    "http://127.0.0.1:$admin_port/healthz" | grep -q '"status":"ok"'
curl --fail --silent --show-error --max-time 2 \
    "http://127.0.0.1:$admin_port/metrics" | grep -q 'maelys_egress_policy_generation 1'

sed 's/127\.0\.0\.1:9/127.0.0.1:8/' "$config" >"$root/reload.conf"
chmod 0600 "$root/reload.conf"
mv "$root/reload.conf" "$config"
kill -HUP "$daemon_pid"
attempt=0
while ! grep -q '"event":"policy-reloaded".*"generation":2' "$root/ops-ready" && test "$attempt" -lt 100; do
    sleep 0.05
    attempt=$((attempt + 1))
done
grep -q '"event":"policy-reloaded".*"generation":2' "$root/ops-ready"

curl --silent --show-error --max-time 2 --noproxy '' \
    --proxy "http://127.0.0.1:$proxy_port" \
    --proxy-header 'Proxy-Authorization: Bearer 0123456789abcdef0123456789abcdef' \
    "http://127.0.0.1:9/" >/dev/null || true
sleep 0.1
test -s "$audit_log"
grep -q '"mac":"[0-9a-f][0-9a-f]*"' "$audit_log"
grep -q 'generation=2' "$audit_log"

kill -TERM "$daemon_pid"
wait "$daemon_pid"
daemon_pid=

if "$binary" serve --listen-unix "$socket" >/dev/null 2>&1; then
    echo "serve accepted direct operational flags" >&2
    exit 1
fi

echo "CLI configuration checks passed"
