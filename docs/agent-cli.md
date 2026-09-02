# Using maelys-egress from an agent

Discover the executable contract instead of guessing flags from prose:

```sh
maelys-egress describe --summary --format json --compact --non-interactive
maelys-egress describe COMMAND_ID --format json --compact --non-interactive
maelys-egress config describe --format json --compact --non-interactive
```

Every call in automation uses `--format json --non-interactive`. Success is
one envelope on stdout; failure is one envelope on stderr with a stable
`error.code` and an actionable `error.hint`. Exit `2` means the command
completed and its report contains violations: read `data`. The generic
agent contract is `agent-cli.md` of the maelys-cli distribution; only Egress
specifics follow.

## Typical sequence

```sh
maelys-egress config validate --config FILE --format json --compact --non-interactive
maelys-egress serve --config FILE --non-interactive
```

Read `data.valid` and, on exit `2`, `data.diagnostics[].code` and
`.message`. On exit `0`, `data.policy.digest` is the SHA-256 policy identity
that `serve` will announce.

`serve` is a protocol-stream command: never pass rendering options to it and
never parse its stdout as an envelope. Set `MAELYS_CLI_FORMAT=json` in its
environment so that a startup failure is one envelope on stderr. Read stdout line by line, wait for
`event == "ready"`, record the listener it selected (`proxy.transport` with
`host` and `port` or `path`, `admin`, `policy.digest`), then keep draining
`receipt`, `policy-reloaded` and `policy-reload-rejected` events until exit.
A `fatal` event or a non-zero exit is a failure; a `serve` that cannot start
writes nothing on stdout and one failure on stderr. The Python and Node.js
process SDKs implement this supervision.

## Product-specific preconditions

- the configuration file must be a regular, non-symlink file owned by root
  or the daemon user and not group/world writable; secret files referenced
  by `token_file` and `audit_key_file` must be owner-only;
- `config describe` reports `data.tlsListener`; TLS listener keys are
  accepted only by a binary built with a TLS module.

For repository changes, agents must follow
[`skills/egress-cli-contract/SKILL.md`](../skills/egress-cli-contract/SKILL.md).
