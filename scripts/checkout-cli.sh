#!/bin/sh
set -eu

destination=${1:-../maelys-cli}
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
tag=$(sed -n '1p' "$root/adapter/MAELYS_CLI_PIN")
pin=$(sed -n '2p' "$root/adapter/MAELYS_CLI_PIN")
if [ -e "$destination" ]; then
    echo "refusing to replace existing path: $destination" >&2
    exit 1
fi
git clone --filter=blob:none --no-checkout \
    https://github.com/maelys-dev/maelys-cli.git "$destination"
git -C "$destination" checkout --detach "$tag"
test "$(git -C "$destination" rev-parse HEAD)" = "$pin"
