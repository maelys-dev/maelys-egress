# @maelys/egress Node.js process SDK

The dependency-free ES module starts and supervises a standalone Egress daemon.
It handles private configuration/credentials, readiness, health, metrics,
policy reload and bounded shutdown; it does not implement an HTTP client.

## Install

From the source tree:

```sh
npm install ./sdk/node
```

From the GitHub release source archive:

```sh
tar -xzf maelys-egress-node-sdk-0.13.0.tar.gz
npm install ./maelys-egress-node-sdk-0.13.0
```

The `maelys-egress` executable must also be installed or passed as an absolute
`binary` option.

## Complete lifecycle example

```js
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { Destination, EgressConfig, EgressProcess } from "@maelys/egress";

const run = promisify(execFile);
const config = new EgressConfig({
  destinations: [
    new Destination("github.com", 443, { requireTlsSni: true }),
    new Destination("api.anthropic.com", 443, { requireTlsSni: true }),
  ],
  maxConnections: 32,
  quotaConnections: 4,
  quotaBytes: 100 * 1024 * 1024,
  quotaTotalBytes: 1024 * 1024 * 1024,
});

const egress = new EgressProcess(config, { binary: "maelys-egress" });
try {
  await egress.start();

  // The SDK operates Egress; curl is the proxy-aware application in this demo.
  await run("curl", ["--fail", "https://github.com/"], {
    env: {
      PATH: process.env.PATH ?? "/usr/bin:/bin",
      HTTPS_PROXY: egress.proxyUrl,
    },
  });

  console.log(await egress.health());
  console.log((await egress.metrics()).maelys_egress_admissions_total);

  const generation = await egress.replaceDestinations([
    new Destination("github.com", 443, { requireTlsSni: true }),
  ]);
  console.log("active generation", generation);
} finally {
  await egress.close();
}
```

`start()` creates the files, launches `maelys-egress serve`, and waits for the
structured `ready` event. The SDK keeps draining lifecycle JSONL afterwards.
`proxyUrl` contains the generated `maelys` credential. `health()` decodes `/healthz`; `metrics()` maps
Prometheus names to numbers. `replaceDestinations()` returns only after the
policy generation advances. `close()` sends `SIGTERM`, escalates after a
deadline if needed, and removes the private directory.

Subscribe to all validated events or a particular event name:

```js
egress.on("lifecycle", (event) => console.log(event.event));
egress.on("receipt", (event) => store(event.receipt));
egress.on("policy-reloaded", (event) => console.log(event.policy.generation));
```

Listeners run on Node's normal event loop. Keep them bounded; durable receipt
storage should queue work rather than blocking the stdout consumer.

Node's built-in `fetch` does not automatically use `HTTP_PROXY` on every
supported Node version. Give `proxyUrl` to a proxy-aware dispatcher/library, or
pass standard variables to a child tool that explicitly supports them:

```js
const childEnvironment = {
  PATH: "/usr/bin:/bin",
  HTTP_PROXY: egress.proxyUrl,
  HTTPS_PROXY: egress.proxyUrl,
  ALL_PROXY: egress.proxyUrl,
};
await run("your-tool", [], { env: childEnvironment });
```

Do not blindly forward the complete host environment to an untrusted child.
Proxy variables provide routing, not confinement; Executor/OS sandboxing must
remove its direct ambient network path.

## Scope and limits

- The SDK creates loopback TCP listeners, not AF_UNIX or remote TLS listeners.
- It manages one standalone principal; use the C ABI for several principals,
  native callbacks, durable audit handles or attestors.
- It does not globally patch Node networking or bundle a proxy client.
- JavaScript strings cannot provide native secure-zero guarantees.
