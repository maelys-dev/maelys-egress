# Licensing

Copyright 2026 David Bromberg.

## Source code: MPL-2.0

The source code of `libmaelys_egress`, the `maelys-egress` command line, the
optional TLS modules, the Python and Node.js process SDKs, the tools, tests,
examples and documentation of this repository is available under the Mozilla
Public License 2.0. The complete terms are in [`LICENSE`](LICENSE); the SDK
packages carry their own copy.

The agent texts of the next section are excluded.

The MPL applies file by file. A program that links `libmaelys_egress.a`,
statically or otherwise, or embeds the daemon, keeps its own license
(section 3.3 of the MPL); only a modified covered file must remain available
in Source Code Form under MPL-2.0. Workloads mediated by the proxy are not
covered software.

## Agent texts: CC-BY-4.0

Every `.claude/skills/*/SKILL.md` of this repository, and the managed
`maelys-cli` and `maelys-release` blocks of `AGENTS.md` and `CLAUDE.md`
together with `docs/maelys-cli-guide.md`, are licensed under CC-BY-4.0 with
attribution to David Bromberg. This holds whether a socle installed the text
or this repository wrote it: what decides is that the text instructs an agent,
not who shipped it. Retain the attribution, and indicate your changes when
sharing an adaptation.

Each text carries its own notice — SPDX identifier, copyright, source and
license link — because an attribution that does not travel with the copy is
not one. `.claude/skills/egress-cli-contract/SKILL.md` is written here and
carries its notice now; the installed texts receive theirs at the next
adoption of the distribution that writes them, never through a pull request
of this repository. Blocks installed before the change were copied under
CC0-1.0 and that grant stands for those copies.

## Redistributed material

[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) lists the pinned Maelys
System and Maelys CLI sources linked into the released artifacts, with their
licenses, and the licenses of the optional, non-vendored TLS libraries.
