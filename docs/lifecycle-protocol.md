# Daemon lifecycle protocol

`maelys-egress serve` writes one JSON object per line to stdout. Prose and
diagnostics never enter this stream.

```text
process starts
     │
     ├── startup failure ─────────────► stderr agent-cli/v2 + exit 1
     │
     └── { event: "ready" }
              │
              ├── { event: "receipt" }                 zero or more
              ├── { event: "policy-reloaded" }         after accepted SIGHUP
              ├── { event: "policy-reload-rejected" }  rejected SIGHUP
              ├── { event: "fatal" } ────────────────► exit 1
              └── { event: "stopping" }
                         └── { event: "stopped" } ───► exit 0
```

Every event begins with:

```json
{"schemaVersion":1,"contract":"maelys-egress-lifecycle/1","event":"..."}
```

## TCP readiness

```json
{
  "schemaVersion": 1,
  "contract": "maelys-egress-lifecycle/1",
  "event": "ready",
  "proxy": {"transport": "tcp", "host": "127.0.0.1", "port": 18080},
  "admin": {"host": "127.0.0.1", "port": 19901},
  "policy": {"generation": 1, "algorithm": "sha256", "digest": "..."}
}
```

For an AF_UNIX listener, `proxy` contains `transport: "unix"` and `path`;
`admin` is `null` when the operations listener is disabled.

## Consumption rule

Do not read only the first line and abandon stdout. Receipt volume can fill the
pipe and stall mediation. Keep one bounded line parser active until process
exit. The shipped SDKs cap a line at 1 MiB, validate the contract/version, make
the readiness event available to the caller, and continue consuming events.

The normative field-level contract is
[`protocol/egress-lifecycle-v1.schema.json`](../protocol/egress-lifecycle-v1.schema.json).
