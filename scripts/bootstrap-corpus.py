"""Materialize reviewable hex seeds as exact binary frames (no newline).

libFuzzer reads these generated seeds, never their textual representation.
New discoveries go to a separate build directory; committed seeds stay fixed.
"""
from pathlib import Path
import sys

source = Path(__file__).resolve().parent.parent / "tests/fuzz/corpus/bootstrap"
destination = Path(sys.argv[1])
destination.mkdir(parents=True, exist_ok=True)
for seed in sorted(source.glob("*.hex")):
    data = bytes.fromhex(seed.read_text())
    assert (data[:4], len(data)) in ((b"MEBQ", 8), (b"MEBP", 16)), seed
    (destination / seed.stem).write_bytes(data)
