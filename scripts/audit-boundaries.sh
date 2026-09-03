#!/bin/sh
# Boundary rules of the repository. Plain POSIX grep -E, so the audit runs
# on any developer machine, CI runner or container without extra packages.
set -eu

# grep_tree PATTERN [grep options...] -- PATH...: recursive extended-regex
# search that prints matches and succeeds when at least one exists.
grep_tree() {
    pattern=$1
    shift
    grep -rnE -e "$pattern" "$@"
}

if grep_tree '#include[[:space:]]*<(openssl|mbedtls|wolfssl|event2|uv|curl)/' \
    src include cli tests; then
    echo "Egress core must not depend on a concrete TLS or event stack" >&2
    exit 1
fi

if grep_tree '(^|[^A-Za-z0-9_])(epoll_|kevent|kqueue[[:space:]]*\(|poll[[:space:]]*\(|select[[:space:]]*\()' src; then
    echo "Egress must consume the maelys-system reactor" >&2
    exit 1
fi

if grep_tree '#include[[:space:]]*[<"]maelys/cli' src include providers tests examples; then
    echo "only the maelys-egress CLI may link maelys-cli; the library must not" >&2
    exit 1
fi

if grep_tree '(^|[^A-Za-z0-9_])(close|pipe|socketpair)[[:space:]]*\(' src; then
    echo "Egress must consume maelys-system descriptor ownership" >&2
    exit 1
fi

# Sockets are created, connected, accepted, read, written and shut down through
# maelys-system handles. The one bare socket is the embedder-owned end of the
# private connector pair in server_listener.c, which System cannot hand over.
if grep_tree '(^|[^A-Za-z0-9_.>])(socket|accept|accept4|connect|bind|listen|recv|send|shutdown|setsockopt)[[:space:]]*\(' \
    --exclude=server_listener.c src; then
    echo "Egress must consume maelys-system sockets" >&2
    exit 1
fi
if grep_tree '(^|[^A-Za-z0-9_.>])(accept|accept4|bind|listen|recv|send|shutdown)[[:space:]]*\(' \
    src/server_listener.c; then
    echo "server_listener.c may only create the embedder-owned connector end bare" >&2
    exit 1
fi

if grep_tree '(^|[^A-Za-z0-9_])(puts|printf|fputs|fputc|fwrite)[[:space:]]*\(|fprintf[[:space:]]*\([[:space:]]*stdout' \
    --exclude=output.c cli; then
    echo "only cli/output.c may write the lifecycle stream to stdout" >&2
    exit 1
fi

if grep -rniE -e 'maelys[-_ ]?netd|(^|[^A-Za-z0-9_])netd([^A-Za-z0-9_]|$)' \
    --exclude-dir=.git --exclude-dir=build --exclude-dir=dist \
    --exclude-dir=node_modules --exclude-dir=__pycache__ \
    --exclude=CHANGELOG.md --exclude=migration-to-egress.md \
    --exclude=audit-boundaries.sh \
    .; then
    echo "the former product namespace escaped the migration guide" >&2
    exit 1
fi

echo "egress boundaries are clean"
