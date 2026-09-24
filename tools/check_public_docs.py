#!/usr/bin/env python3
"""Check stable invariants of the public, hand-written documentation."""

from __future__ import annotations

from pathlib import Path
import re
import sys
from typing import Optional
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[1]
PUBLIC_DOCUMENTS = (
    "README.md",
    "RELEASING.md",
    "LICENSING.md",
    "SECURITY.md",
    "THIRD_PARTY_NOTICES.md",
    "examples/README.md",
    "sdk/README.md",
    "sdk/python/README.md",
    "sdk/node/README.md",
)
# Installed beside them, and it has carried the private name through three
# removals elsewhere; only that check applies, since a changelog links to
# commits and quotes prose the other two rules would reject.
NAME_ONLY_DOCUMENTS = ("CHANGELOG.md",)
PRIVATE_DOCUMENTATION = "maelys-dev/maelys-docs"
LINK = re.compile(r"(?<!!)\[[^\]]+\]\(([^)]+)\)")
DANGLING_WORDS = {
    "a",
    "an",
    "and",
    "as",
    "at",
    "by",
    "for",
    "from",
    "in",
    "into",
    "nor",
    "of",
    "on",
    "or",
    "the",
    "to",
    "with",
    "without",
}


def local_link_target(document: Path, raw_target: str) -> Optional[Path]:
    target = raw_target.strip()
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1]
    else:
        target = target.split(maxsplit=1)[0]
    if target.startswith(("#", "http://", "https://", "mailto:")):
        return None
    target = unquote(target.split("#", 1)[0].split("?", 1)[0])
    return document.parent / target


def prose_blocks(text: str) -> list[tuple[int, str]]:
    blocks: list[tuple[int, str]] = []
    current: list[str] = []
    start = 0
    fenced = False

    def finish() -> None:
        nonlocal current, start
        if current:
            blocks.append((start, " ".join(part.strip() for part in current)))
            current = []

    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith(("```", "~~~")):
            finish()
            fenced = not fenced
            continue
        if fenced:
            continue
        structural = (
            not stripped
            or stripped.startswith(("#", "- ", "* ", "+ ", ">", "|", "<!--"))
            or re.match(r"\d+[.)]\s", stripped) is not None
        )
        if structural:
            finish()
            continue
        if not current:
            start = number
        current.append(stripped)
    finish()
    return blocks


def main() -> int:
    failures: list[str] = []
    for relative in PUBLIC_DOCUMENTS:
        document = ROOT / relative
        text = document.read_text(encoding="utf-8")
        if PRIVATE_DOCUMENTATION in text:
            failures.append(f"{relative}: names a private documentation repository")
        for match in LINK.finditer(text):
            target = local_link_target(document, match.group(1))
            if target is not None and not target.exists():
                line = text.count("\n", 0, match.start()) + 1
                failures.append(
                    f"{relative}:{line}: local Markdown link does not exist: {match.group(1)}"
                )
        for line, paragraph in prose_blocks(text):
            words = re.findall(r"[A-Za-z]+", paragraph)
            if words and words[-1].lower() in DANGLING_WORDS:
                failures.append(
                    f"{relative}:{line}: prose paragraph ends with dangling word '{words[-1]}'"
                )
    for relative in NAME_ONLY_DOCUMENTS:
        if PRIVATE_DOCUMENTATION in (ROOT / relative).read_text(encoding="utf-8"):
            failures.append(f"{relative}: names a private documentation repository")
    if failures:
        print("public documentation checks failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print("public documentation checks passed "
          f"({len(PUBLIC_DOCUMENTS)} files, {len(NAME_ONLY_DOCUMENTS)} for the private name)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
