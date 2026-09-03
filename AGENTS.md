# Agent instructions

The public CLI is an executable protocol shared by humans, scripts, SDKs and
LLMs. Before adding or changing a command, option, configuration key, exit
status or lifecycle event, read and follow
[`skills/egress-cli-contract/SKILL.md`](skills/egress-cli-contract/SKILL.md).

Do not hand-edit files under `docs/generated/`. Update the central catalog or
protocol schema, run `make cli-reference`, and commit the generated result.

No backward-compatibility alias should be introduced without an explicit
product decision. The 0.x series prefers one clear contract over parallel old
and new spellings.

<!-- maelys-cli:begin -->
# Maelys CLI framework (maelys-cli 0.5.1)

This project builds its command-line interface on `libmaelys_cli`. The
complete guide is in `docs/maelys-cli-guide.md`; this block is the summary
that must hold for every change.

## Using a Maelys CLI from an agent

- Start with `PROGRAM describe --summary --format json --compact --non-interactive`,
  then `PROGRAM describe COMMAND_ID --format json` before invoking a command.
  Treat `input` and `outputSchema` as one public contract; never build a call
  from the human help text.
- Always pass `--format json --non-interactive` in automation. `--json` is an
  exact alias of `--format json`; `--compact` keeps one line.
- Exit `0` is success, `1` is an execution failure, `2` is a completed
  validation report that found violations. Success data is on stdout only;
  failures are a JSON envelope on stderr with a stable `error.code` and an
  actionable `error.hint`.
- Transactional commands plan by default and write only with `--apply`.
  Review the plan, then repeat the same invocation with `--apply`.
  `--dry-run` and `--plan` are rejected.
- Never pass rendering flags to a command whose `outputMode` is
  `protocol-stream`; its stdout belongs to the declared protocol. Set
  `MAELYS_CLI_FORMAT=json` in the environment to receive its failure
  envelope as JSON on stderr.
- `describe COMMAND_ID` is minimal; `describe --summary` lists everything.
  A descriptor with `available: false` names a command this build cannot
  run (`unavailableReason`). Operands may carry `type` and `choices` like
  options; `input.constraints` includes `requires`, `at-most-one` and
  `all-or-none` groups.
- `PROGRAM completion bash|zsh|fish` prints the shell completion generated
  from the catalog.
- Unknown, duplicated or foreign options are refused. Fix the invocation
  instead of retrying it.

## Adding or changing a CLI action

One command is one entry of the central catalog (`maelys_cli_command_t`),
one handler and one JSON Schema file. In the same change, update:

1. the catalog entry, written with the declaration macros
   (`MAELYS_CLI_READ`, `_RECORDS`, `_TRANSACTION`, `_EXECUTE`, `_STREAM`,
   `_PROTOCOL_STREAM`, `_EXTERNAL`; `MAELYS_CLI_OPERAND`, `_OPERAND_OPTIONAL`,
   `_OPERAND_REST`, `_OPERAND_CHOICE`, `_OPERAND_KIND`; `MAELYS_CLI_FLAG`,
   `_STRING`, `_PATH`, `_ABSOLUTE_PATH`, `_UNSIGNED`, `_INTEGER`, `_SIZE`,
   `_DURATION`, `_CHOICE`, `_HEX`, `_HEX_OR`, `_DIGEST`) plus `.required`,
   `.repeatable`, `.depends_on`, `.depends_on_all`, `.conflicts_with`,
   `.group` (all-or-none) and `.default_text` (validated at startup, returned
   by the typed accessors: never repeat a default in the handler); a command
   this build cannot provide declares `.unavailable = "reason"`;
2. the output schema: a JSON Schema file under the project's schema
   directory, embedded by `maelys-cli-embed` and referenced with
   `MAELYS_CLI_SCHEMA(symbol)`; never a hand-escaped C string;
3. the handler, which reads only through `maelys_cli_operand*()`,
   `maelys_cli_option*()` and `maelys_cli_flag()` (typed kinds replace
   hand validation of paths, digests and choices), replies exactly once
   through `maelys_cli_succeed*()`, `maelys_cli_emit_record*()` +
   `maelys_cli_finish_records()` or `maelys_cli_fail*()`, and tests
   `maelys_cli_replied()` after a helper that may have replied; optional
   helper programs are found with `maelys_cli_resolve_helper()`;
4. focused tests: accepted and refused inputs, plan without write, `--apply`
   with write, error codes and exit codes, `describe COMMAND_ID` exposing the
   exact contract;
5. the generated CLI reference when the project keeps one.

Linking: a product CLI links `libmaelys_cli.a` only (no dependency). A
dispatcher that runs external commands from manifests adds
`libmaelys_cli_extension.a` and one `libmaelys-json.a` (pkg-config
`maelys-cli-extension`, CMake `maelys::cli_extension`); never embed a
dependency archive into your own `.a`. Reading untrusted JSON is
maelys-json's job, not the framework's; the framework only writes JSON
and refuses invalid UTF-8 in what it writes.

Rules that must not be broken: no second usage string outside the catalog; no
hand-written argv parsing in `main()`; no product type inside the shared
framework; validation errors in causal order (command, options, values,
dependencies, operands, files, syntax, schema, state); explicit
`MAELYS_CLI_WRITE_REPLACE` / `MAELYS_CLI_WRITE_NO_REPLACE` on every file
write; external programs started with absolute paths and `execve`, never a
shell or PATH lookup.
<!-- maelys-cli:end -->

<!-- maelys-release:begin -->
# Maelys release socle (maelys-release v0.2.7)

This repository publishes through the shared maelys-release workflows. The
rules below hold for every release-related change; the complete conventions
are in `docs/conventions.md` of maelys-release.

- `.github/workflows/release.yml` is generated by `scripts/adopt.sh` of
  maelys-release. Never edit it by hand; run `adopt.sh DIR --apply` from a
  maelys-release checkout at the wanted tag to upgrade, and
  `adopt.sh DIR --check` (exit 2 on drift) to verify.
- A release is a signed, annotated tag `vX.Y.Z` on `main` whose commit
  carries `VERSION` = `X.Y.Z` and a dated `CHANGELOG.md` entry. Never push a
  tag before `make check` passes on that exact commit, never move or force a
  tag, never publish from a branch.
- The workflow verifies the tag through the GitHub API, builds on Linux
  x86_64, Linux arm64 and macOS arm64 with `scripts/package-release.sh
  TARGET`, attests provenance, publishes the GitHub release, renders
  `packaging/homebrew/maelys-egress.rb.in` from the tag's own copy, builds
  bottles when configured and pushes the formula to `maelys-dev/homebrew-tap`.
- Formula names: a command is named after its binary (`maelys-egress`), a
  library after its archive with a `lib` prefix (`libmaelys-sys`). Dependency
  pins in a formula are copied from the tag's `adapter/` files, never typed.
- Tap credentials are the repository secrets `HOMEBREW_TAP_TOKEN` and
  `HOMEBREW_TAP_SIGNING_KEY`; without them the tap job renders, lints and
  reports instead of failing. Never commit a secret or a key.
- Runners are JSON inputs of the workflow; public repositories use
  GitHub-hosted runners only. A self-hosted runner is reserved for hardware
  gates, on signed tags or `workflow_dispatch`, behind the `release`
  environment.
- A tag whose release exists but whose formula or bottles failed is
  replayed with `gh workflow run release.yml -f tag=vX.Y.Z` after adopting
  a corrected socle; a tag is never moved or recreated.
<!-- maelys-release:end -->
