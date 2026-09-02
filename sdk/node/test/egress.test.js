import assert from "node:assert/strict";
import { statSync } from "node:fs";
import test from "node:test";

import { Destination, EgressConfig, EgressProcess } from "../index.js";

test("IPv6 proxy URLs are bracketed", () => {
  const egress = new EgressProcess(new EgressConfig({
    destinations: [new Destination("127.0.0.1", 9, { allowPrivate: true })],
    listenHost: "::1",
    adminHost: "::1",
  }));
  egress.secret = "secret";
  egress.proxyPort = 8080;
  assert.equal(egress.proxyUrl, "http://maelys:secret@[::1]:8080");
});

test("lifecycle, health, metrics and policy reload", async () => {
  const config = new EgressConfig({
    destinations: [new Destination("127.0.0.1", 9, { allowPrivate: true })],
  });
  const egress = new EgressProcess(config, {
    binary: process.env.MAELYS_EGRESS_BINARY,
    stderr: "inherit",
  });
  const lifecycle = [];
  egress.on("lifecycle", (event) => lifecycle.push(event));
  try {
    await egress.start();
    assert.equal(lifecycle[0].contract, "maelys-egress-lifecycle/1");
    assert.equal(lifecycle[0].event, "ready");
    assert.equal(statSync(egress.directory).mode & 0o777, 0o700);
    assert.equal(statSync(`${egress.directory}/token`).mode & 0o777, 0o600);
    assert.equal(statSync(`${egress.directory}/egress.conf`).mode & 0o777, 0o600);
    assert.equal(egress.child.spawnargs.join(" ").includes(egress.secret), false);
    assert.equal((await egress.health()).status, "ok");
    assert.match(egress.proxyUrl, /^http:\/\/maelys:/);
    assert.equal((await egress.metrics()).maelys_egress_policy_generation, 1);
    const generation = await egress.replaceDestinations([
      new Destination("127.0.0.1", 8, { allowPrivate: true }),
    ]);
    assert.equal(generation, 2);
    assert.equal(lifecycle.some((event) => event.event === "policy-reloaded"), true);
  } finally {
    await egress.close();
  }
});

test("invalid configuration and failed start clean up", async () => {
  assert.throws(() => new EgressConfig({ destinations: [] }));
  assert.throws(() => new Destination("127.0.0.1", 443, {
    allowPrivate: true,
    requireTlsSni: true,
  }));
  const egress = new EgressProcess(new EgressConfig({
    destinations: [new Destination("127.0.0.1", 9, { allowPrivate: true })],
  }), { binary: "/definitely/missing/maelys-egress" });
  await assert.rejects(egress.start());
  assert.equal(egress.directory, null);
  await egress.close();
});
