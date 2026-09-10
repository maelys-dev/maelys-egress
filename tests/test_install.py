#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Install under two prefixes without cleaning, and check what landed.

The dispatcher manifest and the pkg-config file name the installation, not the
sources. Nothing in the source tree changes between the two installs below, so
a rule that decided from prerequisites would leave the first prefix in place.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
PREFIXES = ("/usr/local", "/opt/maelys-audit")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    build = Path(sys.argv[1]).resolve()
    overrides = list(sys.argv[2:])
    binary = build / "bin/maelys-egress"
    manifest = build / "share/maelys/commands/egress.json"
    pc = build / "lib/pkgconfig/maelys-egress.pc"

    environment = dict(os.environ)
    for name in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL"):
        environment.pop(name, None)
    make = [shutil.which("make"), "--no-print-directory", f"BUILD={build}", *overrides]

    def invoke(*arguments):
        done = subprocess.run([*make, *arguments], cwd=ROOT, env=environment,
                              capture_output=True, text=True)
        if done.returncode:
            raise AssertionError(done.stdout + done.stderr)

    built = digest(binary)
    with tempfile.TemporaryDirectory(prefix="install-test-", dir=build) as staging:
        for prefix in PREFIXES:
            invoke("install", f"PREFIX={prefix}", f"DESTDIR={staging}")
            stage = Path(staging) / prefix.lstrip("/")
            installed = stage / "bin/maelys-egress"
            declared = json.loads(
                (stage / "share/maelys/commands/egress.json").read_text(encoding="utf-8"))

            assert declared["executable"] == f"{prefix}/bin/maelys-egress", declared
            assert declared["sha256"] == built == digest(installed), (
                f"manifest {declared['sha256']}, built {built}, "
                f"installed {digest(installed)}")
            assert len(declared["sha256"]) == 64, declared["sha256"]
            reported = subprocess.run([installed, "--version"], capture_output=True,
                                      text=True, check=True).stdout.split()[-1]
            assert declared["version"] == reported, (
                f"manifest declares {declared['version']}, the binary reports {reported}")
            assert declared["command"] == "egress", declared
            assert (stage / "lib/pkgconfig/maelys-egress.pc").read_text(
                encoding="utf-8").splitlines()[0] == f"prefix={prefix}", prefix
            assert installed.stat().st_mode & 0o777 == 0o755, oct(installed.stat().st_mode)
            shutil.rmtree(stage)

        # Rendering again for an unchanged prefix must not touch the files, or
        # everything downstream of them would rebuild on every invocation.
        invoke("install-metadata", f"PREFIX={PREFIXES[-1]}")
        stamps = {path: path.stat().st_mtime_ns for path in (manifest, pc)}
        invoke("install-metadata", f"PREFIX={PREFIXES[-1]}")
        for path, before in stamps.items():
            assert path.stat().st_mtime_ns == before, f"{path.name} was rewritten"

    # Leave the build tree rendered for the PREFIX the caller asked for.
    invoke("install-metadata")
    print(f"install-metadata-check: manifest and pkg-config follow PREFIX; "
          f"manifest binds {built[:12]}…")


if __name__ == "__main__":
    main()
