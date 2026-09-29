#!/bin/sh
set -eu

socket=/run/maelys-egress/egress.sock
test -S "$socket"

# The listening socket exists only on the loopback interface of the app's
# network namespace. Bytes are relayed unchanged; Egress still authenticates
# and parses the HTTP proxy request on the AF_UNIX side.
exec socat \
    TCP4-LISTEN:3128,bind=127.0.0.1,reuseaddr,fork \
    UNIX-CONNECT:"$socket"
