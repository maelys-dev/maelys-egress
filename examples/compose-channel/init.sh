#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
set -eu

# Separate UIDs; group 20000 owns the socket capability. The application can
# traverse this setgid directory but cannot create, replace or unlink entries.
install -d -o 10001 -g 20000 -m 2750 /run/maelys-egress
install -d -o 10001 -g 10001 -m 0700 /run/maelys-config
# Normalize host bind-mount ownership for Linux and Docker Desktop alike.
install -o 10001 -g 10001 -m 0600 /example/egress.conf /run/maelys-config/egress.conf
# No bearer token, private key or other secret is created or shared.
