#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Render the pkg-config file and the dispatcher manifest for one PREFIX.

Both name the installation, not the sources: PREFIX and the linker flags can
change without any source timestamp changing, so make cannot decide from
prerequisites whether these files are current. They are rendered on every
invocation instead, and identical bytes keep their mtime so nothing that
depends on them rebuilds.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def publish(path, content):
    """Replace a generated file atomically, leaving unchanged bytes alone."""
    path = Path(path)
    content = content.encode("utf-8")
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(handle, "wb") as output:
            output.write(content)
            os.fchmod(output.fileno(), 0o644)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as binary:
        for block in iter(lambda: binary.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("prefix", "version", "system-version", "binary", "pkgconfig",
                 "manifest"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    if not Path(args.prefix).is_absolute() or any(c in args.prefix for c in "\n\r\0"):
        parser.error("prefix must be an absolute, single-line path")

    pc = (ROOT / "pkgconfig/maelys-egress.pc.in").read_text(encoding="utf-8")
    for name, value in (("PREFIX", args.prefix), ("VERSION", args.version),
                        ("SYSTEM_VERSION", args.system_version)):
        pc = pc.replace(f"@{name}@", value)

    manifest = json.loads((ROOT / "cli/command.json.in").read_text(encoding="utf-8"))
    manifest["executable"] = str(Path(args.prefix) / "bin/maelys-egress")
    manifest["version"] = args.version
    manifest["sha256"] = sha256_file(args.binary)

    publish(args.pkgconfig, pc)
    publish(args.manifest, json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
