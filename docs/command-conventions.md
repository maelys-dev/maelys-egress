# Maelys Egress command conventions

The `maelys-egress` command line is a public protocol built on
`libmaelys_cli`. The framework conventions apply in full: declarative
catalog, `describe`, `agent-cli/v2` envelopes, causal validation, stable
error codes and exit statuses `0`, `1` and `2`. They are documented in
`command-conventions.md` of the maelys-cli distribution
(`PREFIX/share/maelys-cli/docs/`, or `docs/` of the checkout pinned by
`adapter/MAELYS_CLI_PIN`). This document keeps only what is specific to
Egress.

## Grammar

```text
maelys-egress COMMAND [SUBCOMMAND] [OPERANDS] [OPTIONS]
```

`help`, `version`, `describe` and `completion` are the framework built-ins.
The product commands are `config describe`, `config validate` and `serve`.

`serve` and `config validate` accept one complete configuration file, never
operational policy or listener flags:

```sh
maelys-egress config validate --config /etc/maelys-egress.conf
maelys-egress serve --config /etc/maelys-egress.conf
```

This avoids two policy grammars and ambiguous merge precedence. The file must
declare `schema_version = 1`; `config describe` publishes every key, type,
default, range, dependency and cross-key constraint.

## Output channels

```text
stdout       result data or the declared protocol stream
stderr       diagnostics only
exit status  0 completed, 1 failed, 2 validation report with violations
```

| Command | Protocol | Notes |
| --- | --- | --- |
| `serve` | `maelys-egress-lifecycle/1` JSON Lines | stdout belongs to the stream from `ready` to exit; rendering options are refused; drain until the process exits |

Every lifecycle line is independently parseable and conforms to
`protocol/egress-lifecycle-v1.schema.json`. Receipts and reload events
continue after `ready`. `fatal`, or a non-zero exit, is a failure. A `serve`
that cannot start writes nothing on stdout and reports one failure on stderr,
as text by default or as an `agent-cli/v2` envelope when the environment
sets `MAELYS_CLI_FORMAT=json`; the process SDKs set it.

## Shell completion

`maelys-egress completion bash|zsh|fish` prints the completion script
generated from the catalog by the framework; it completes command words,
options and `--config` values.

## Commands with a validation report

`config validate` completes with exit `2` and `data.valid == false` when the
file content cannot become a running daemon: syntax, unknown or duplicate
keys, cross-key constraints, unreadable or untrusted secret files, TLS keys
in a binary without a TLS module, unresolvable destinations or refused
listener modes. `data.diagnostics[]` carries the stable code, the message
and a hint. Exit `1` with an error envelope is reserved for a configuration
file that cannot be read at all (`NOT_FOUND`, `ACCESS_DENIED`, `IO_FAILED`).

Exit `0` means the file, credentials, policy and backend-specific settings
can be constructed without opening a listener; `data.policy.digest` is the
SHA-256 identity the daemon will report in `ready`. It does not promise that a
port will still be free later.

## Error codes used by Egress

| Code | Egress boundary |
| --- | --- |
| `VALIDATION_FAILED` | invocation, configuration syntax, values, cross-key constraints |
| `POLICY_FAILED` | a destination cannot be resolved, pinned or sealed |
| `ACCESS_DENIED` | untrusted configuration or secret file, refused authentication or listener mode |
| `IO_FAILED` | a listener cannot bind, the audit journal cannot be opened |
| `PRECONDITION_FAILED` | daemon state refuses the operation |
| `UNSUPPORTED` | TLS listener keys in a binary without a TLS module |
| `UNEXPECTED` | allocation or signal-handling failure |

## Sources of truth

- `cli/main.c` declares the commands, operands, options, effects and output
  modes with the `MAELYS_CLI_*` macros;
- `cli/config_catalog.c` declares the configuration keys and constraints,
  and `cli/config_file.c` applies them into one typed settings structure
  shared by `serve`, `config validate` and SIGHUP reload;
- `cli/schemas/*.json` define `data` of every finite command and are
  embedded at build time by `maelys-cli-embed`;
- `protocol/agent-cli-v2.schema.json` defines the finite envelope and
  `protocol/egress-lifecycle-v1.schema.json` the daemon events;
- `docs/generated/cli-reference.md`, `docs/generated/cli-contract.json` and
  `docs/generated/config-reference.md` are generated from `describe` of a
  release-neutral build (version `0.0.0`) by `make cli-reference`;
  `make contract-check`, part of `make check`, rejects a stale copy;
- `make schema-check`, also part of `make check`, validates the envelopes,
  `data` objects and a complete lifecycle run of the built binary against
  those schemas.
