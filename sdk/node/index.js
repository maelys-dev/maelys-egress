import { spawn } from "node:child_process";
import { randomBytes } from "node:crypto";
import { EventEmitter } from "node:events";
import { closeSync, mkdtempSync, openSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { get } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";

const LIFECYCLE_CONTRACT = "maelys-egress-lifecycle/1";

export class Destination {
  constructor(host, port, { allowPrivate = false, requireTlsSni = false } = {}) {
    if (typeof host !== "string" || host.length === 0 || /\s/.test(host)) {
      throw new TypeError("destination host must be non-empty without whitespace");
    }
    if (!Number.isInteger(port) || port < 1 || port > 65535) {
      throw new RangeError("destination port must be in 1..65535");
    }
    if (allowPrivate && requireTlsSni) {
      throw new TypeError(
        "the standalone format has no combined private-address/SNI directive",
      );
    }
    this.host = host;
    this.port = port;
    this.allowPrivate = allowPrivate;
    this.requireTlsSni = requireTlsSni;
    Object.freeze(this);
  }

  directive() {
    const key = this.requireTlsSni
      ? "allow_tls_sni"
      : this.allowPrivate
        ? "allow_private"
        : "allow";
    return `${key} = ${this.host}:${this.port}`;
  }
}

export class EgressConfig {
  constructor({
    destinations,
    listenHost = "127.0.0.1",
    listenPort = 0,
    adminHost = "127.0.0.1",
    adminPort = 0,
    maxConnections = 128,
    quotaConnections = 0,
    quotaBytes = 0,
    quotaTotalBytes = 0,
  }) {
    if (!Array.isArray(destinations) || destinations.length === 0 ||
        !destinations.every((item) => item instanceof Destination)) {
      throw new TypeError("at least one Destination is required");
    }
    if (!["127.0.0.1", "::1"].includes(listenHost) ||
        !["127.0.0.1", "::1"].includes(adminHost)) {
      throw new TypeError("the process SDK creates loopback listeners only");
    }
    for (const [name, value, maximum] of [
      ["listenPort", listenPort, 65535],
      ["adminPort", adminPort, 65535],
      ["maxConnections", maxConnections, 1_000_000],
      ["quotaConnections", quotaConnections, 1_000_000],
      ["quotaBytes", quotaBytes, Number.MAX_SAFE_INTEGER],
      ["quotaTotalBytes", quotaTotalBytes, Number.MAX_SAFE_INTEGER],
    ]) {
      if (!Number.isSafeInteger(value) || value < 0 || value > maximum) {
        throw new RangeError(`invalid ${name}`);
      }
    }
    if (maxConnections === 0) throw new RangeError("maxConnections must be positive");
    this.destinations = Object.freeze([...destinations]);
    this.listenHost = listenHost;
    this.listenPort = listenPort;
    this.adminHost = adminHost;
    this.adminPort = adminPort;
    this.maxConnections = maxConnections;
    this.quotaConnections = quotaConnections;
    this.quotaBytes = quotaBytes;
    this.quotaTotalBytes = quotaTotalBytes;
    Object.freeze(this);
  }

  withDestinations(destinations) {
    return new EgressConfig({
      destinations,
      listenHost: this.listenHost,
      listenPort: this.listenPort,
      adminHost: this.adminHost,
      adminPort: this.adminPort,
      maxConnections: this.maxConnections,
      quotaConnections: this.quotaConnections,
      quotaBytes: this.quotaBytes,
      quotaTotalBytes: this.quotaTotalBytes,
    });
  }

  render(tokenPath) {
    return [
      "schema_version = 1",
      `listen = ${this.listenHost}:${this.listenPort}`,
      `admin_listen = ${this.adminHost}:${this.adminPort}`,
      `token_file = ${tokenPath}`,
      `max_connections = ${this.maxConnections}`,
      `quota_connections = ${this.quotaConnections}`,
      `quota_bytes = ${this.quotaBytes}`,
      `quota_total_bytes = ${this.quotaTotalBytes}`,
      ...this.destinations.map((destination) => destination.directive()),
      "",
    ].join("\n");
  }
}

function writePrivate(path, content) {
  const descriptor = openSync(path, "wx", 0o600);
  try {
    writeFileSync(descriptor, content, { encoding: "utf8" });
  } finally {
    closeSync(descriptor);
  }
}

function sleep(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function urlHost(host) {
  return host.includes(":") ? `[${host}]` : host;
}

function adminGet(host, port, path) {
  return new Promise((resolve, reject) => {
    const request = get({ host, port, path, timeout: 2000 }, (reply) => {
      const chunks = [];
      let length = 0;
      reply.on("data", (chunk) => {
        length += chunk.length;
        if (length > 1024 * 1024) {
          request.destroy(new Error("maelys-egress admin response exceeds 1 MiB"));
          return;
        }
        chunks.push(chunk);
      });
      reply.on("end", () => {
        if (reply.statusCode !== 200) {
          reject(new Error(`maelys-egress admin status ${reply.statusCode}`));
        } else {
          resolve(Buffer.concat(chunks));
        }
      });
    });
    request.on("timeout", () => request.destroy(new Error("maelys-egress admin timeout")));
    request.on("error", reject);
  });
}

export class EgressProcess extends EventEmitter {
  constructor(config, {
    binary = "maelys-egress",
    startupTimeoutMs = 10_000,
    stderr = "inherit",
  } = {}) {
    super();
    if (!(config instanceof EgressConfig)) throw new TypeError("EgressConfig required");
    this.config = config;
    this.binary = binary;
    this.startupTimeoutMs = startupTimeoutMs;
    this.stderr = stderr;
    this.child = null;
    this.directory = null;
    this.secret = null;
    this.proxyPort = 0;
    this.adminPort = 0;
    this.policyDigest = "";
    this.listenIdentity = "";
    this.lifecycleError = null;
    this.#stdoutBuffer = Buffer.alloc(0);
  }

  #stdoutBuffer;

  async start() {
    if (this.child) throw new Error("maelys-egress process already started");
    this.#stdoutBuffer = Buffer.alloc(0);
    this.lifecycleError = null;
    this.directory = mkdtempSync(join(tmpdir(), "maelys-egress-sdk-"));
    this.secret = randomBytes(32).toString("base64url");
    const tokenPath = join(this.directory, "token");
    const configPath = join(this.directory, "egress.conf");
    writePrivate(tokenPath, this.secret);
    writePrivate(configPath, this.config.render(tokenPath));
    try {
      this.child = spawn(this.binary, [
        "serve", "--config", configPath, "--non-interactive",
      ], {
        stdio: ["ignore", "pipe", this.stderr],
      });
      await this.#waitReady();
      if (this.proxyPort === 0 || this.adminPort === 0) {
        throw new Error("daemon did not allocate TCP proxy/admin ports");
      }
      return this;
    } catch (error) {
      await this.close();
      throw error;
    }
  }

  #waitReady() {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => finish(new Error("maelys-egress readiness timeout")),
                               this.startupTimeoutMs);
      const finish = (error) => {
        clearTimeout(timer);
        this.off("ready", onReady);
        this.off("lifecycle-fatal", onFatal);
        this.child?.off("error", onError);
        this.child?.off("exit", onExit);
        if (error) reject(error); else resolve();
      };
      const onReady = () => finish();
      const onFatal = (event) => finish(new Error(event.message ?? "maelys-egress failed"));
      const onError = (error) => finish(error);
      const onExit = (status) => finish(new Error(`maelys-egress exited before ready (${status})`));
      this.on("ready", onReady);
      this.on("lifecycle-fatal", onFatal);
      this.child.on("error", onError);
      this.child.on("exit", onExit);
      this.child.stdout.on("data", (chunk) => this.#consumeLifecycle(chunk));
    });
  }

  #consumeLifecycle(chunk) {
    this.#stdoutBuffer = Buffer.concat([this.#stdoutBuffer, chunk]);
    if (this.#stdoutBuffer.length > 1 << 20 && this.#stdoutBuffer.indexOf(0x0a) < 0) {
      this.lifecycleError = new Error("maelys-egress lifecycle line exceeds 1 MiB");
      this.emit("lifecycle-fatal", { message: this.lifecycleError.message });
      this.child?.kill("SIGTERM");
      return;
    }
    for (;;) {
      const newline = this.#stdoutBuffer.indexOf(0x0a);
      if (newline < 0) return;
      if (newline > 1 << 20) {
        this.lifecycleError = new Error("maelys-egress lifecycle line exceeds 1 MiB");
        this.emit("lifecycle-fatal", { message: this.lifecycleError.message });
        this.child?.kill("SIGTERM");
        return;
      }
      const line = this.#stdoutBuffer.subarray(0, newline);
      this.#stdoutBuffer = this.#stdoutBuffer.subarray(newline + 1);
      try {
        const event = JSON.parse(line.toString("utf8"));
        if (event.schemaVersion !== 1 || event.contract !== LIFECYCLE_CONTRACT ||
            typeof event.event !== "string") {
          throw new Error("invalid maelys-egress lifecycle event");
        }
        if (event.event === "ready") {
          if (event.proxy?.transport !== "tcp" || typeof event.proxy.host !== "string" ||
              !Number.isInteger(event.proxy.port) || !Number.isInteger(event.admin?.port) ||
              typeof event.policy?.digest !== "string") {
            throw new Error("invalid ready lifecycle event");
          }
          this.listenIdentity = event.proxy.host;
          this.proxyPort = event.proxy.port;
          this.adminPort = event.admin.port;
          this.policyDigest = event.policy.digest;
        }
        this.emit("lifecycle", event);
        this.emit(event.event === "fatal" ? "lifecycle-fatal" : event.event, event);
      } catch (error) {
        this.lifecycleError = error;
        this.emit("lifecycle-fatal", { message: error.message });
        this.child?.kill("SIGTERM");
        return;
      }
    }
  }

  get proxyUrl() {
    if (!this.secret || this.proxyPort === 0) throw new Error("maelys-egress is not ready");
    const host = urlHost(this.config.listenHost);
    return `http://maelys:${encodeURIComponent(this.secret)}@${host}:${this.proxyPort}`;
  }

  async health() {
    return JSON.parse((await adminGet(
      this.config.adminHost, this.adminPort, "/healthz",
    )).toString("utf8"));
  }

  async metrics() {
    const result = {};
    const text = (await adminGet(
      this.config.adminHost, this.adminPort, "/metrics",
    )).toString("ascii");
    for (const line of text.split("\n")) {
      if (!line || line.startsWith("#")) continue;
      const [name, value] = line.trim().split(/\s+/, 2);
      result[name] = Number(value);
    }
    return result;
  }

  async replaceDestinations(destinations, { timeoutMs = 5000 } = {}) {
    if (!this.child || !this.directory) throw new Error("maelys-egress is not ready");
    const previous = (await this.metrics()).maelys_egress_policy_generation;
    const candidate = this.config.withDestinations(destinations);
    const replacement = join(this.directory, "egress.conf.new");
    writePrivate(replacement, candidate.render(join(this.directory, "token")));
    renameSync(replacement, join(this.directory, "egress.conf"));
    this.child.kill("SIGHUP");
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const generation = (await this.metrics()).maelys_egress_policy_generation;
      if (generation > previous) {
        this.config = candidate;
        return generation;
      }
      await sleep(20);
    }
    throw new Error("policy generation did not advance after SIGHUP");
  }

  async close() {
    const child = this.child;
    this.child = null;
    if (child && child.exitCode === null && child.signalCode === null) {
      await new Promise((resolve) => {
        const force = setTimeout(() => {
          if (child.exitCode === null) child.kill("SIGKILL");
        }, 5000);
        child.once("exit", () => {
          clearTimeout(force);
          resolve();
        });
        child.kill("SIGTERM");
      });
    }
    if (this.directory) rmSync(this.directory, { recursive: true, force: true });
    this.directory = null;
    this.secret = null;
    this.proxyPort = 0;
    this.adminPort = 0;
  }
}
