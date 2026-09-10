# maelys-egress Python process SDK

The dependency-free Python helper starts and supervises a standalone Egress
daemon. It is useful when a test runner, agent host or local application wants
an execution-scoped proxy without managing token files, readiness, reload and
shutdown itself.

## Install

From the source tree:

```sh
python3 -m pip install ./sdk/python
```

From the GitHub release source archive:

```sh
tar -xzf maelys-egress-python-sdk-0.19.2.tar.gz
python3 -m pip install ./maelys-egress-python-sdk-0.19.2
```

The SDK never searches the inherited `PATH`. It finds `maelys-egress` beside
the Python interpreter or in a fixed system installation directory; otherwise,
pass `binary="/absolute/path/maelys-egress"`.

Automatic discovery runs a candidate only when nobody but root or the
calling user could have replaced it: the file, the file it resolves to and
every directory on both paths must be owned by root or by the caller, must
not be writable by everyone and may be group-writable only when the caller
owns them; a sticky directory such as `/tmp` is exempt from the two write
checks, since only an entry's owner may replace it there. A candidate that fails the
rule is skipped and named in the final `FileNotFoundError`. A process
running with more privileges than the owner of a Homebrew prefix therefore
never picks the binary from that prefix; it must pass an explicit `binary`.
An explicit `binary` is trusted as given; `binary_trust_refusal(path)`
returns why a path fails the same rule, or `None`, for callers that want to
apply it themselves.

## Complete lifecycle example

```python
import os
import subprocess

from maelys_egress import Destination, EgressConfig, EgressProcess

config = EgressConfig([
    Destination("github.com", 443, require_tls_sni=True),
    Destination("api.anthropic.com", 443, require_tls_sni=True),
], max_connections=32, quota_connections=4,
   quota_bytes=100 * 1024 * 1024,
   quota_total_bytes=1024 * 1024 * 1024)

with EgressProcess(config) as egress:
    # The SDK operates Egress; curl is the proxy-aware application in this demo.
    client_environment = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "HTTPS_PROXY": egress.proxy_url,
    }
    subprocess.run(
        ["curl", "--fail", "https://github.com/"],
        env=client_environment,
        check=True,
    )

    print(egress.health())
    print(egress.metrics()["maelys_egress_admissions_total"])

    generation = egress.replace_destinations([
        Destination("github.com", 443, require_tls_sni=True),
    ])
    print("active generation", generation)
```

`EgressProcess.start()` creates the credential/configuration, starts
`maelys-egress serve`, and waits for the structured `ready` lifecycle event.
Its background reader continues draining receipts and reload events for the
whole process lifetime. `proxy_url` includes the generated `maelys` credential and can be
given to a compatible client. `health()` returns decoded `/healthz` JSON over a direct loopback connection
that ignores the proxy environment and follows no redirect;
`metrics()` returns metric names mapped to numbers. `replace_destinations()`
rewrites only the destination policy, sends `SIGHUP`, and returns after the
generation advances. The context manager always calls `close()`.

Read validated lifecycle events without parsing daemon stdout yourself:

```python
event = egress.next_event(timeout=2.0)
print(event["event"])
```

Alternatively pass `on_event=lambda event: ...` to `EgressProcess`. The callback
runs on the lifecycle-reader thread and should return quickly.

Retention is bounded and never blocks the reader of daemon stdout. With
`on_event`, the callback is the consumer and nothing is retained
(`next_event()` raises `RuntimeError`). Without it, at most
`max_pending_events` events (default 1024) wait for `next_event()`; when a
new event arrives on a full queue the oldest waiting event is dropped and
`dropped_events` counts the losses, `pending_events` the backlog. Size the
bound for the receipts a run can produce between two reads, or consume
through the callback.

For a child tool that honors proxy environment variables:

```python
environment = {
    "PATH": "/usr/bin:/bin",
    "HTTP_PROXY": egress.proxy_url,
    "HTTPS_PROXY": egress.proxy_url,
    "ALL_PROXY": egress.proxy_url,
}
subprocess.run(["your-tool"], env=environment, check=True)
```

Add required non-sensitive variables explicitly; blindly copying the entire
host environment is usually inappropriate for an untrusted workload.
Proxy variables alone are not a sandbox—the child must also have no direct
ambient network route.

## Scope and limits

- The SDK intentionally creates loopback TCP listeners, not AF_UNIX or remote
  TLS listeners. Use the CLI/configuration file or C ABI for those topologies.
- It manages one principal and policy. Use the C ABI for multiple invocation
  identities, callbacks, durable audit handles or attestors.
- It does not alter Python's global networking behavior. Configure the chosen
  HTTP client explicitly.
- The generated credential exists as a Python string and cannot be reliably
  wiped by the interpreter.
