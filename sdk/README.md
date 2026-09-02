# Process SDKs

## Why they exist

Egress already has a C embedding ABI and a standalone daemon. Python and Node.js
applications often need the standalone security boundary but should not each
reimplement its process lifecycle.

The process SDKs automate this sequence:

```text
application
    │ create EgressConfig
    ▼
SDK creates private directory + token + strict config
    │ spawn maelys-egress serve --config ... --non-interactive
    │ validate and continuously drain lifecycle JSONL
    ▼
application receives proxy URL, health and metrics helpers
    │ optional atomic policy reload
    ▼
SDK sends bounded shutdown and removes private files
```

They are **not proxy clients**. They do not send the application's web request,
parse HTTP/SOCKS or replace the application's normal networking library. The
application gives `proxy_url`/`proxyUrl` to a proxy-aware HTTP client, curl, or
a child process through `HTTP_PROXY`, `HTTPS_PROXY` and `ALL_PROXY`.

They are also not native bindings: Egress remains a separate process. Use the C
ABI when the mediator must run inside the application process, needs several
principals, a receipt callback, custom TLS provider or HSM attestor.

## What both SDKs guarantee

- a fresh mode-0700 temporary directory;
- mode-0600 token and configuration files;
- no token in the Egress command-line arguments;
- a bounded startup deadline and a 1 MiB bound per lifecycle JSON line;
- continuous contract/version validation and draining of receipts/reload events;
- loopback-only proxy and operations listeners;
- `/healthz` and `/metrics` helpers;
- atomic destination replacement followed by `SIGHUP` and generation check;
- `SIGTERM`, then bounded `SIGKILL` fallback and private-file cleanup.

The SDK creates one standalone principal named `maelys`. JavaScript and Python
strings cannot be reliably secure-zeroed, so use Executor's native adapter when
credential lifetime must stay inside a native trusted host.

## Distribution

Release assets contain source archives. They are not registry publications and
no PyPI/npm trust relationship is claimed. Extract the matching archive and
install the local directory, or use the source tree directly:

```sh
python3 -m pip install ./sdk/python
npm install ./sdk/node
```

See the [Python guide](python/README.md) and [Node.js guide](node/README.md).
