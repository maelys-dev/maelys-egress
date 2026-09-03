#!/bin/sh
# Render packaging/homebrew/maelys-egress.rb.in for one released tag.
# usage: scripts/render-homebrew-formula.sh vX.Y.Z [OUTPUT [FORMULA]]
# FORMULA defaults to maelys-egress, the only formula of this repository.
# The source archive of the tag is downloaded to compute its digest, and the
# maelys-cli pin is read from the tag's own adapter/ file, so the formula
# builds the framework the release was verified with. Maelys System comes
# from the tap's libmaelys-sys formula.
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
tag=${1:?usage: render-homebrew-formula.sh vX.Y.Z [OUTPUT [FORMULA]]}
output=${2:-$root/dist/homebrew/maelys-egress.rb}
formula=${3:-maelys-egress}
test "$formula" = maelys-egress || { echo "unknown formula: $formula" >&2; exit 64; }
repository=${MAELYS_SOURCE_REPOSITORY:-maelys-dev/maelys-egress}
version=${tag#v}
url="https://github.com/$repository/archive/refs/tags/$tag.tar.gz"

temp_base=${TMPDIR:-/tmp}
temp_base=${temp_base%/}
work=$(mktemp -d "$temp_base/maelys-egress-formula.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
curl -fsSL --retry 5 --retry-delay 3 -o "$work/source.tar.gz" "$url"
digest=$(shasum -a 256 "$work/source.tar.gz" | awk '{print $1}')
mkdir -p "$work/tag"
tar -xzf "$work/source.tar.gz" -C "$work/tag" --strip-components=1
cli_tag=$(sed -n '1p' "$work/tag/adapter/MAELYS_CLI_PIN")
cli_pin=$(sed -n '2p' "$work/tag/adapter/MAELYS_CLI_PIN")
test "$(cat "$work/tag/VERSION")" = "$version" || {
    echo "tag $tag carries VERSION $(cat "$work/tag/VERSION")" >&2
    exit 1
}
mkdir -p "$(dirname "$output")"
sed -e "s|@URL@|$url|g" -e "s|@VERSION@|$version|g" -e "s|@SHA256@|$digest|g" \
    -e "s|@CLI_TAG@|$cli_tag|g" \
    -e "s|@CLI_PIN@|$cli_pin|g" \
    "$work/tag/packaging/homebrew/maelys-egress.rb.in" >"$output"
grep -q '@[A-Z_]*@' "$output" && { echo "unrendered placeholder in $output" >&2; exit 1; }
printf '%s\n' "rendered $output"
