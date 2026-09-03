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
tar -xzf maelys-egress-python-sdk-0.13.0.tar.gz
python3 -m pip install ./maelys-egress-python-sdk-0.13.0
```

The `maelys-egress` executable must also be installed or supplied with
`binary="/absolute/path/maelys-egress"`.

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

with EgressProcess(config, binary="maelys-egress") as egress:
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
given to a compatible client. `health()` returns decoded `/healthz` JSON;
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
