#!/bin/sh
set -eu

destination=${1:-../maelys-system}
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
pin=$(cat "$root/adapter/MAELYS_SYSTEM_PIN")
if [ -e "$destination" ]; then
    echo "refusing to replace existing path: $destination" >&2
    exit 1
fi
git clone --filter=blob:none --no-checkout \
    https://github.com/maelys-dev/maelys-system.git "$destination"
git -C "$destination" checkout --detach "$pin"
test "$(git -C "$destination" rev-parse HEAD)" = "$pin"
