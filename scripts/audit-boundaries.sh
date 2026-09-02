#!/bin/sh
set -eu

command -v rg >/dev/null 2>&1 || {
    echo "ripgrep is required for the boundary audit" >&2
    exit 1
}

if rg -n '#include[[:space:]]*<(openssl|mbedtls|wolfssl|event2|uv|curl)/' \
    src include cli tests; then
    echo "Egress core must not depend on a concrete TLS or event stack" >&2
    exit 1
fi

if rg -n '\b(epoll_|kevent|kqueue\s*\(|poll\s*\(|select\s*\()' src; then
    echo "Egress must consume the maelys-system reactor" >&2
    exit 1
fi

if rg -n '#include[[:space:]]*[<"]maelys/cli' src include providers tests examples; then
    echo "only the maelys-egress CLI may link maelys-cli; the library must not" >&2
    exit 1
fi

if rg -n '\b(close|pipe|socketpair)\s*\(' src; then
    echo "Egress must consume maelys-system descriptor ownership" >&2
    exit 1
fi

if rg -n '\b(puts|printf|fputs|fputc|fwrite)\s*\(|fprintf\s*\(\s*stdout' cli --glob '!cli/output.c'; then
    echo "only cli/output.c may write the lifecycle stream to stdout" >&2
    exit 1
fi

if rg -n -i 'maelys[-_ ]?netd|\bnetd\b' \
    --hidden \
    --glob '!.git/**' --glob '!build/**' --glob '!dist/**' \
    --glob '!sdk/python/build/**' --glob '!sdk/node/node_modules/**' \
    --glob '!CHANGELOG.md' --glob '!docs/migration-to-egress.md' \
    --glob '!scripts/audit-boundaries.sh' \
    .; then
    echo "the former product namespace escaped the migration guide" >&2
    exit 1
fi

echo "egress boundaries are clean"
