#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

case "$(uname -s)" in Linux) host_os=linux ;; Darwin) host_os=macos ;; *) exit 1 ;; esac
case "$(uname -m)" in x86_64|amd64) host_arch=x86_64 ;; arm64|aarch64) host_arch=arm64 ;; *) exit 1 ;; esac
target="${1:-${host_os}-${host_arch}}"
case "$target" in linux-x86_64|linux-arm64|macos-arm64) ;; *) echo "unsupported target" >&2; exit 1 ;; esac
test "$target" = "${host_os}-${host_arch}" || { echo "native packaging only" >&2; exit 1; }

version="${PACKAGE_VERSION_OVERRIDE:-$(cat VERSION)}"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "invalid version" >&2; exit 1; }
dist="$root/dist"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$dist"
find "$dist" -maxdepth 1 -type f -name "*${version}*" -delete

make clean
make VERSION="$version" check
stage="$tmp/stage"
make VERSION="$version" install DESTDIR="$stage" PREFIX=/usr/local
cat >"$tmp/smoke.c" <<'EOF'
#include <maelys/egress.h>
int main(void) { return MAELYS_EGRESS_ABI_VERSION == 2u ? 0 : 1; }
EOF
"${CC:-cc}" -std=c11 -I"$stage/usr/local/include" "$tmp/smoke.c" \
  -L"$stage/usr/local/lib" -lmaelys_egress -lmaelys_sys -pthread -o "$tmp/smoke"
"$tmp/smoke"
test "$("$stage/usr/local/bin/maelys-egress" version)" = "maelys-egress $version"
system_version="$(sed -n '1p' adapter/MAELYS_SYSTEM_PIN)"
grep -Fq "Version: ${system_version#v}" "$stage/usr/local/lib/pkgconfig/maelys-sys.pc"
grep -Fq '"command": "egress"' "$stage/usr/local/share/maelys/commands/egress.json"

tar_name="maelys-egress-${version}-${target}.tar.gz"
tar -czf "$dist/$tar_name" -C "$stage" .
(cd "$dist" && sha256 "$tar_name" >"${tar_name}.sha256")

if [ "$target" = linux-x86_64 ]; then
  python_sdk="maelys-egress-python-sdk-${version}"
  node_sdk="maelys-egress-node-sdk-${version}"
  mkdir -p "$tmp/$python_sdk" "$tmp/$node_sdk"
  cp sdk/python/LICENSE sdk/python/README.md sdk/python/pyproject.toml \
    "$tmp/$python_sdk/"
  cp -R sdk/python/src sdk/python/tests "$tmp/$python_sdk/"
  cp sdk/node/LICENSE sdk/node/README.md sdk/node/index.js sdk/node/package.json \
    "$tmp/$node_sdk/"
  cp -R sdk/node/test "$tmp/$node_sdk/"
  tar -czf "$dist/${python_sdk}.tar.gz" -C "$tmp" "$python_sdk"
  tar -czf "$dist/${node_sdk}.tar.gz" -C "$tmp" "$node_sdk"
  (cd "$dist" && sha256 "${python_sdk}.tar.gz" >"${python_sdk}.tar.gz.sha256")
  (cd "$dist" && sha256 "${node_sdk}.tar.gz" >"${node_sdk}.tar.gz.sha256")
fi

if [ "$host_os" = macos ]; then
  test "$(lipo -archs "$stage/usr/local/lib/libmaelys_egress.a")" = arm64
  test "$(lipo -archs "$stage/usr/local/lib/libmaelys_sys.a")" = arm64
  exit 0
fi

command -v dpkg-deb >/dev/null
command -v rpmbuild >/dev/null
case "$host_arch" in x86_64) deb_arch=amd64; rpm_arch=x86_64 ;; arm64) deb_arch=arm64; rpm_arch=aarch64 ;; esac

linux_stage="$tmp/linux-stage"
make VERSION="$version" install DESTDIR="$linux_stage" PREFIX=/usr
deb_root="$tmp/deb"
cp -a "$linux_stage" "$deb_root"
mkdir -p "$deb_root/DEBIAN"
installed_size="$(du -sk "$deb_root/usr" | awk '{print $1}')"
cat >"$deb_root/DEBIAN/control" <<EOF
Package: maelys-egress
Version: ${version}
Section: net
Priority: optional
Architecture: ${deb_arch}
Installed-Size: ${installed_size}
Maintainer: Maelys Developers <noreply@maelys.dev>
Depends: libc6
Description: policy-enforced HTTP and SOCKS network mediator
 Includes the daemon, public static C SDK and pinned Maelys System foundation.
EOF
deb_name="maelys-egress_${version}_${deb_arch}.deb"
dpkg-deb --root-owner-group --build "$deb_root" "$dist/$deb_name" >/dev/null
(cd "$dist" && sha256 "$deb_name" >"${deb_name}.sha256")

rpm_top="$tmp/rpmbuild"
mkdir -p "$rpm_top"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
spec="$rpm_top/SPECS/maelys-egress.spec"
cat >"$spec" <<EOF
Name:           maelys-egress
Version:        ${version}
Release:        1
Summary:        Policy-enforced HTTP and SOCKS network mediator
License:        MPL-2.0
URL:            https://github.com/maelys-dev/maelys-egress
BuildArch:      ${rpm_arch}

%description
Daemon, public static C SDK and pinned Maelys System foundation.

%prep
%build
%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -a ${linux_stage}/. %{buildroot}/

%files
/usr/bin/maelys-egress
/usr/include/maelys/egress.h
/usr/include/maelys/egress_tls.h
/usr/include/maelys/egress_profile.h
/usr/include/maelys/sys.h
/usr/include/maelys/sys/
/usr/lib/libmaelys_egress.a
/usr/lib/libmaelys_sys.a
/usr/lib/pkgconfig/maelys-egress.pc
/usr/lib/pkgconfig/maelys-sys.pc
/usr/share/doc/maelys-egress/
/usr/share/maelys/commands/egress.json
EOF
rpmbuild --define "_topdir $rpm_top" -bb "$spec" >/dev/null
rpm_source="$(find "$rpm_top/RPMS" -type f -name '*.rpm' -print -quit)"
test -n "$rpm_source"
rpm_name="$(basename "$rpm_source")"
cp "$rpm_source" "$dist/$rpm_name"
(cd "$dist" && sha256 "$rpm_name" >"${rpm_name}.sha256")

ls -1 "$dist"/*"${version}"*
