import assert from "node:assert/strict";
import { chmodSync, mkdirSync, mkdtempSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import {
  Destination, EgressConfig, EgressProcess, binaryTrustRefusal,
} from "../index.js";

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
  assert.throws(() => new EgressProcess(new EgressConfig({
    destinations: [new Destination("127.0.0.1", 9, { allowPrivate: true })],
  }), { binary: "maelys-egress" }), /absolute path/);
  const egress = new EgressProcess(new EgressConfig({
    destinations: [new Destination("127.0.0.1", 9, { allowPrivate: true })],
  }), { binary: "/definitely/missing/maelys-egress" });
  await assert.rejects(egress.start());
  assert.equal(egress.directory, null);
  await egress.close();
});

test("a replaceable binary is refused by the trust rule", () => {
  const directory = mkdtempSync(join(tmpdir(), "maelys-egress-trust-"));
  try {
    const binDir = join(directory, "bin");
    mkdirSync(binDir, { mode: 0o755 });
    const binary = join(binDir, "maelys-egress");
    writeFileSync(binary, "#!/bin/sh\nexit 0\n", { mode: 0o755 });
    assert.throws(() => binaryTrustRefusal("maelys-egress"), /absolute path/);
    // Owned by the caller, writable by nobody else: trusted.
    assert.equal(binaryTrustRefusal(binary), null);
    // The owner's group is the owner's decision.
    chmodSync(binary, 0o775);
    assert.equal(binaryTrustRefusal(binary), null);
    // Anyone may replace the file: refused, and the refusal is named.
    chmodSync(binary, 0o777);
    assert.match(binaryTrustRefusal(binary), /writable by everyone/);
    // Anyone may replace the directory entry: refused as well.
    chmodSync(binary, 0o755);
    chmodSync(binDir, 0o777);
    assert.match(binaryTrustRefusal(binary), new RegExp(binDir.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")));
    // A sticky world-writable directory only lets owners replace their own
    // entries: trusted.
    chmodSync(binDir, 0o1777);
    assert.equal(binaryTrustRefusal(binary), null);
    // A sticky file gets no exemption.
    chmodSync(binary, 0o1777);
    assert.match(binaryTrustRefusal(binary), /writable by everyone/);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
