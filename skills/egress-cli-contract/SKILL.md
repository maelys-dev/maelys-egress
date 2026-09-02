---
name: egress-cli-contract
description: Change Maelys Egress CLI commands, options, configuration keys, lifecycle events, generated references, or process SDK consumers while preserving their shared executable contract.
---

# Maelys Egress CLI contract

Treat the CLI as a protocol consumed simultaneously by humans, shell scripts,
LLMs and the Python/Node process SDKs. It is built on the pinned
`libmaelys_cli` framework (`adapter/MAELYS_CLI_PIN`): the envelope, error
codes, exit statuses, `describe` shape and global options are the
framework's and are not redefined here.

## Before changing the surface

1. Run `build/release/bin/maelys-egress describe --format json --compact` and,
   for configuration work, `config describe --format json --compact`.
2. Classify the change: `read` envelope, validation report (exit 2) or
   `stream`. State its stdout contract, stderr diagnostics and exit statuses.
3. Declare it first. Commands, operands and options live in `cli/main.c`
   with the `MAELYS_CLI_*` macros; the `data` shape of a finite command lives
   in `cli/schemas/*.json`; configuration keys live in
   `cli/config_catalog.c` and are applied to `egress_cli_settings_t` in
   `cli/config_file.c`, where value and cross-key checks belong. A public
   surface without a declaration is incomplete.
4. Configuration remains the only operational input to `serve` and
   `config validate`; do not add duplicate command-line policy or listener
   flags.

## Required invariants

- Finite commands return `agent-cli/v2` through `maelys_cli_succeed*` and
  `maelys_cli_fail*` only: success on stdout, failure on stderr, never both.
- Error codes are the eleven stable framework codes; Egress maps library
  results in `cli/serve.c` (`result_code`) and never invents a code.
- Exit `2` is a completed validation report with violations, never an
  error envelope. `config validate` reports content problems this way and
  reserves exit `1` for an unreadable configuration file.
- `serve` stdout contains JSONL conforming to
  `protocol/egress-lifecycle-v1.schema.json`; it never contains prose or an
  envelope. Its failures before `ready` go to stderr through the framework.
- Diagnostics never contain credentials, tokens or audit keys.
- Keep TLS-only configuration keys absent from `config describe` for a binary
  built without a TLS provider, and refuse them with `UNSUPPORTED`.
- Preserve ABI 2 unless the public C headers actually change.
- The Egress library never includes `maelys/cli`, and only `cli/output.c`
  writes to stdout; `scripts/audit-boundaries.sh` enforces both.

## Finish the change

Update the handlers in `cli/commands.c`, the shell test
`tests/test_cli.sh`, then the Python and Node SDKs when lifecycle or
configuration rendering changes. Run:

```sh
make cli-reference
make contract-check
make lifecycle-contract-check
make schema-check
make sdk-check
make check
```

Commit the regenerated `docs/generated/` files with their catalog change. Do
not patch them by hand. They come from a release-neutral build (version
`0.0.0`), so a release bump never rewrites them. Advancing `adapter/MAELYS_CLI_PIN` is a product
decision recorded in the changelog, never a side effect of a command change.
