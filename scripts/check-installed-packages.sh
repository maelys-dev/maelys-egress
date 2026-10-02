#!/bin/sh
# Installs the .deb and the .rpm that scripts/package-release.sh wrote, each
# in a clean container of the distribution it is for, and uses what was
# installed. Inspecting the contents of a package says what it holds, not
# that it installs nor that the installed command runs: until 0.27.0 no gate
# installed either, and the first real install was done by hand.
#
# usage: scripts/check-installed-packages.sh DIST_DIR VERSION
#
# Needs docker, and packages built for the architecture of this machine.
set -eu
dist=$(CDPATH='' cd -- "${1:?usage: check-installed-packages.sh DIST_DIR VERSION}" && pwd)
version=${2:?usage: check-installed-packages.sh DIST_DIR VERSION}
deb_image=${MAELYS_EGRESS_DEB_IMAGE:-ubuntu:26.04}
rpm_image=${MAELYS_EGRESS_RPM_IMAGE:-fedora:latest}

case "$(uname -m)" in
    x86_64|amd64) deb_arch=amd64 rpm_arch=x86_64 ;;
    aarch64|arm64) deb_arch=arm64 rpm_arch=aarch64 ;;
    *) echo "check-installed-packages: unknown architecture $(uname -m)" >&2; exit 1 ;;
esac
deb="maelys-egress_${version}_${deb_arch}.deb"
rpm="maelys-egress-${version}-1.${rpm_arch}.rpm"
test -f "$dist/$deb" || { echo "check-installed-packages: $dist/$deb is missing" >&2; exit 1; }
test -f "$dist/$rpm" || { echo "check-installed-packages: $dist/$rpm is missing" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/maelys-egress-packages.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
# What runs inside the container, after the install: the command answers
# with the packaged version, its catalog has the commands, the manifest's
# digest is the installed binary's, the SDK is there, and a program started
# through the installed command finds its channel and returns its status.
cat >"$work/use.sh" <<'INSIDE'
set -eu
version=$1
test "$(maelys-egress --version)" = "maelys-egress $version"
binary=$(command -v maelys-egress)
catalog=$(maelys-egress describe --summary --format json --compact --non-interactive)
for id in config.validate serve channel.broker channel.exec; do
    printf '%s' "$catalog" | grep -q "\"id\":\"$id\"" ||
        { echo "the installed catalog lacks $id" >&2; exit 1; }
done
digest=$(sha256sum "$binary" | cut -d' ' -f1)
grep -q "$digest" /usr/share/maelys/commands/egress.json ||
    { echo "the installed manifest does not name the installed binary's digest" >&2; exit 1; }
for file in /usr/include/maelys/egress.h /usr/include/maelys/egress_client.h \
        /usr/lib/libmaelys_egress.a /usr/lib/libmaelys_egress_client.a \
        /usr/lib/pkgconfig/maelys-egress.pc; do
    test -f "$file" || { echo "the package did not install $file" >&2; exit 1; }
done
mkdir -m 700 /configuration
printf 'schema_version = 1\nchannel_principal = installed\nallow_private = 127.0.0.1:9\n' \
    >/configuration/egress.conf
maelys-egress config validate --config /configuration/egress.conf \
    --format json --non-interactive >/dev/null
status=0
maelys-egress channel exec --config /configuration/egress.conf --non-interactive -- \
    /bin/sh -c 'test "$MAELYS_EGRESS_CHANNEL_FD" = 4 && exit 7' || status=$?
test "$status" = 7 ||
    { echo "channel exec of the installed command returned $status, not the program's 7" >&2; exit 1; }
INSIDE

docker run --rm -v "$dist:/packages:ro" -v "$work:/check:ro" "$deb_image" \
    sh -c "dpkg -i /packages/$deb >/dev/null && sh /check/use.sh $version"
echo "check-installed-packages: $deb installs and runs on $deb_image"
docker run --rm -v "$dist:/packages:ro" -v "$work:/check:ro" "$rpm_image" \
    sh -c "rpm -i /packages/$rpm && sh /check/use.sh $version"
echo "check-installed-packages: $rpm installs and runs on $rpm_image"
