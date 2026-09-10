# Licensing

Copyright 2026 David Bromberg.

## Source code: MPL-2.0

The source code of `libmaelys_egress`, the `maelys-egress` command line, the
optional TLS modules, the Python and Node.js process SDKs, the tools, tests,
examples and documentation of this repository is available under the Mozilla
Public License 2.0. The complete terms are in [`LICENSE`](LICENSE); the SDK
packages carry their own copy.

The MPL applies file by file. A program that links `libmaelys_egress.a`,
statically or otherwise, or embeds the daemon, keeps its own license
(section 3.3 of the MPL); only a modified covered file must remain available
in Source Code Form under MPL-2.0. Workloads mediated by the proxy are not
covered software.

## Installed agent texts: CC-BY-4.0

The managed `maelys-cli` and `maelys-release` blocks of `AGENTS.md` and
`CLAUDE.md`, `docs/maelys-cli-guide.md` and the skills under `.claude/`,
are installed by `maelys agents install` and `maelys-release adopt` from
the Maelys CLI and maelys-release distributions. Their `share/agents/`
texts are licensed under CC-BY-4.0 with attribution to David Bromberg.
Blocks installed before that change were copied under CC0-1.0 and that
grant stands for those copies; the notices identifying copyright, source
and license arrive with the next adoption. Retain them, and indicate your
changes when sharing an adaptation. This applies to the installed blocks,
not to what this repository writes outside them.

## Redistributed material

[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) lists the pinned Maelys
System and Maelys CLI sources linked into the released artifacts, with their
licenses, and the licenses of the optional, non-vendored TLS libraries.
