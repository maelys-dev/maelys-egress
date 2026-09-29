#!/bin/sh
set -eu

token=$(cat /run/maelys-secrets/app-token)
test "${#token}" -ge 16

# Fail closed if the application unexpectedly has an ambient route. This is a
# deployment assertion, not something Egress itself can establish.
unset HTTPS_PROXY HTTP_PROXY https_proxy http_proxy ALL_PROXY all_proxy NO_PROXY no_proxy
if curl --fail --silent --show-error --noproxy '*' \
    --connect-timeout 1 --max-time 2 \
    "${TARGET_URL:-https://github.com/robots.txt}" >/dev/null 2>&1; then
    echo "the application unexpectedly reached the destination without Egress" >&2
    exit 1
fi

# Hex credentials are URL-safe. A real application may read the same secret
# into its own proxy configuration rather than constructing these variables.
HTTPS_PROXY="http://maelys:${token}@127.0.0.1:3128"
HTTP_PROXY=$HTTPS_PROXY
https_proxy=$HTTPS_PROXY
http_proxy=$HTTPS_PROXY
export HTTPS_PROXY HTTP_PROXY https_proxy http_proxy
unset ALL_PROXY all_proxy NO_PROXY no_proxy

# The bridge starts after Egress is healthy, but Compose can start this process
# first because the bridge joins this service's network namespace. Retrying a
# refused loopback connection makes that ordering explicit and bounded.
curl --fail --silent --show-error \
    --retry 30 --retry-delay 1 --retry-connrefused --retry-all-errors \
    --connect-timeout 2 --max-time 10 \
    "${TARGET_URL:-https://github.com/robots.txt}" >/tmp/response

test -s /tmp/response
printf 'request succeeded through Maelys Egress (%s bytes)\n' "$(wc -c </tmp/response)"
