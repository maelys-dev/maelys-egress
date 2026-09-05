#!/bin/sh
# Build and install Egress against an installed Maelys System instead of the
# pinned checkout, the way the Homebrew formula does. The pinned checkout is
# installed into a scratch prefix first, so the gate needs the same inputs
# as every other one.
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
system_dir=${MAELYS_SYSTEM_DIR:-$root/../maelys-system}
system_version=$(sed -n '1p' "$root/dependencies/maelys-system.pin")
system_version=${system_version#v}
temp_base=${TMPDIR:-/tmp}
temp_base=${temp_base%/}
work=$(mktemp -d "$temp_base/maelys-egress-installed.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

make -C "$system_dir" BUILD="$work/system-build" VERSION="$system_version" CPPFLAGS= \
    PREFIX="$work/prefix" install >/dev/null
test -f "$work/prefix/lib/libmaelys_sys.a"

make -C "$root" BUILD_PROFILE=installed-system \
    MAELYS_SYSTEM_PREFIX="$work/prefix" MAELYS_CLI_DIR="${MAELYS_CLI_DIR:-$root/../maelys-cli}" \
    PREFIX="$work/egress" install >/dev/null
test -x "$work/egress/bin/maelys-egress"
test ! -e "$work/egress/lib/libmaelys_sys.a"
test ! -e "$work/egress/include/maelys/sys.h"
grep -Fq "\"executable\": \"$work/egress/bin/maelys-egress\"" \
    "$work/egress/share/maelys/commands/egress.json"
grep -Fq "Requires.private: maelys-sys >= $system_version" \
    "$work/egress/lib/pkgconfig/maelys-egress.pc"
"$work/egress/bin/maelys-egress" version | grep -Fq "maelys-egress $(cat "$root/VERSION")"

# An older installed System must be refused.
sed -i.bak "s/^#define MAELYS_SYS_VERSION \".*\"/#define MAELYS_SYS_VERSION \"0.0.1\"/" \
    "$work/prefix/include/maelys/sys/version.h"
if make -C "$root" BUILD_PROFILE=installed-system-old \
    MAELYS_SYSTEM_PREFIX="$work/prefix" check-system-contract >/dev/null 2>&1; then
    echo "an older installed maelys-system was accepted" >&2
    exit 1
fi
rm -rf "$root/build/installed-system" "$root/build/installed-system-old"
echo "installed-system check: ok"
