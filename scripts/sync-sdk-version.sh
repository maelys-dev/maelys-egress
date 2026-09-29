#!/bin/sh
# The SDKs shipped with a release carry VERSION, including their install
# examples. Called by maelys-release cut after it writes VERSION.
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
version=$(sed -n '1p' "$root/VERSION")
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || exit 1
temporary=
trap 'test -z "$temporary" || rm -f "$temporary"' EXIT HUP INT TERM
rewrite() {
    file="$root/$1"
    temporary=$(mktemp "$file.XXXXXX")
    sed -E "$2" "$file" > "$temporary"
    chmod 0644 "$temporary"
    if ! cmp -s "$file" "$temporary"; then mv "$temporary" "$file"; fi
    rm -f "$temporary"
    temporary=
}
rewrite sdk/python/pyproject.toml "s/^version = \"[^\"]*\"/version = \"$version\"/"
rewrite sdk/python/src/maelys_egress/__init__.py "s/^__version__ = \"[^\"]*\"/__version__ = \"$version\"/"
rewrite sdk/node/package.json "s/^(  \"version\": )\"[^\"]*\"/\1\"$version\"/"
for sdk in python node; do
    rewrite "sdk/$sdk/README.md" "s/(maelys-egress-$sdk-sdk-)[0-9]+\.[0-9]+\.[0-9]+/\1$version/g"
done
