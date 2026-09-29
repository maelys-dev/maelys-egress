#!/bin/sh
set -eu

# Compose chooses these deployment identities explicitly. It does not depend on
# the unspecified numeric ID of the user baked into the Egress runtime image.
egress_uid=10001
egress_gid=10001
app_uid=10002
app_gid=10002

install -d -o "$egress_uid" -g "$egress_gid" -m 0700 /run/maelys-egress
install -d -o "$egress_uid" -g "$egress_gid" -m 0700 /run/maelys-config
install -d -o 0 -g 0 -m 0711 /run/maelys-secrets

# Linux bind mounts preserve the host owner's numeric UID, which is neither
# root nor the runtime UID on a hosted runner. Materialize a trusted copy so
# the configuration has the same ownership on Docker Desktop and Linux.
install -o "$egress_uid" -g "$egress_gid" -m 0600 \
    /example/egress.conf /run/maelys-config/egress.conf

# Generate one credential at deployment time. Egress and the application get
# distinct owner-only inodes because the CLI refuses a secret not owned by its
# own effective UID. Both identities are fixed by this deployment example.
token=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
test "${#token}" -eq 64
umask 077
printf '%s\n' "$token" >/run/maelys-secrets/egress-token
printf '%s\n' "$token" >/run/maelys-secrets/app-token
chown "$egress_uid:$egress_gid" /run/maelys-secrets/egress-token
chown "$app_uid:$app_gid" /run/maelys-secrets/app-token
chmod 0600 /run/maelys-secrets/egress-token /run/maelys-secrets/app-token
