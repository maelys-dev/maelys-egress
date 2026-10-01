# `maelys-egress channel exec`

**Status: implemented.** `channel exec` is in `cli/channel_exec.c`, tested by
`tests/test_channel_exec.py` (`make channel-exec-check`) and held by four
mutants of `scripts/mutation-check.sh`. This document decided the contract
before any code; what the implementation changed in it is listed at the end.
The command changes no wire byte and no public C declaration: it is built on
`maelys_egress_channel_create`, which exists since 0.22.0, so neither ABI
number moves. maelys-cli provides the process functions it needs since
0.5.31.

## Why

A program written for the mediated-connection channel receives it in one of
two ways. It opens it by pathname through a broker
([bootstrap v1](../protocol/egress-channel-bootstrap-v1.md)), or it inherits
it as an open descriptor, which is the convention maelys-warden uses with
descriptor 4. Outside Warden, nothing launches a program of the second kind:
[`channel_supervisor.c`](../examples/channel_supervisor.c) shows the gesture
(`fork`, `dup2`, `execve`) and every user would write it again, with its
ordering, signal and exit-code mistakes.

`channel exec` is that supervisor, written once:

```
maelys-egress channel exec --config FILE -- /absolute/program [ARGUMENT...]
```

It starts a server with no listener, creates one channel, starts the program
with the channel on an inherited descriptor, and exits with the program's
status. No socket appears on disk, no readiness is polled, and the server
lives exactly as long as the program.

The channel specification left this to "a version of the command, not of
this protocol" ([channel v1](../protocol/egress-channel-v1.md), *Not in this
document*), and the bootstrap proposal already describes it as independent:
an anonymous channel inherited directly needs no bootstrap and no lease.

## What it is not

**It is not a sandbox.** The program keeps every network path its
environment gives it; the channel adds a mediated path and removes none.
The command is a launcher for programs that choose to connect through
Egress, and a way to test one under a policy with receipts. Confinement is
the deployer's: a container with `network_mode: none`, Warden, or another
supervisor. The reference documentation says so in its first paragraph,
because the application summary reads "forward proxy for sandboxed
workloads" and the name invites the wrong reading.

Also out of scope: Docker and OCI integration, a client helper that reads the
environment, any escalation to `SIGKILL`, restarting the program, and more
than one channel or one program per invocation.

## Decisions

### 1. Output: the program owns stdout and stderr

The command is a `stream` command with `outputMode: "protocol-stream"` and
**no** `protocol`: agent-cli-spec v2.6.0 section 2 says that "a command that
merely relays a child's stdio declares none". It therefore emits none of the
`maelys-egress-lifecycle/1` records that `serve` and `channel broker` write.

Two consequences in the existing code:

- `egress_cli_receipt_sink` is installed on every server and writes to
  stdout (`cli/serve.c`, `cli/output.c`). In this mode no receipt sink is
  installed. Receipts reach the operator through `audit_log` only. That is a
  loss compared with the two daemon commands, and it is deliberate; a
  receipt file outside the standard streams can be added later as a key
  without changing this contract.
- A policy reload reports its outcome through the lifecycle stream today. In
  this mode it is one diagnostic line on stderr.

Before the program starts, a failure of Egress is the JSON error envelope on
stderr, like any command. After it starts, Egress writes to stderr only what
the operator must know (a reload outcome, a fatal server error, a cleanup
failure), each on one line prefixed `maelys-egress:`.

### 2. Exit status: the program's, once it has started

agent-cli-spec section 8 already decides this: "A stream command or a
delegate propagates the exit status of the underlying process, `128 +
signal` on signal termination." There is nothing to negotiate with the
specification, and `exitCodes` in `describe` stays exactly `{0, 1, 2}`: the
framework emits it and the conformance kit compares it by equality. The
propagation is stated in the command's `purpose` and in the application's
`agent_guidance`, which today names only `serve` and `channel broker`.

The rule that removes the ambiguity is temporal:

- **Not started** — configuration refused, server or channel not created,
  program not trusted, `exec` failed: error envelope on stderr, exit 1.
- **Started** — the exit status is the program's, whatever happens next. A
  failed cleanup or a server that died while the program ran is a line on
  stderr and never replaces the status. A program that exits 0 is reported
  0.

Reserved launcher codes in the manner of 125, 126 and 127 are rejected: the
specification fixes 1 for an execution failure, and a reserved code proves
nothing since the program may return it too. The envelope is the only
reliable mark of a failure of Egress, and the temporal rule says when one
can exist.

This needs the framework to return from the launch only once `exec` has
succeeded or failed, which `maelys_cli_process_run` already does internally
through its error pipe.

### 3. How the program finds the channel

- The program is the operand after `--`, an **absolute path**, checked by
  the framework as it checks every external program (regular file, not
  writable by group or others, trusted parent, no `env` shebang). The
  remaining operands are its arguments, passed verbatim.
- The descriptor number is the configuration key `channel_fd`, default
  **4**, accepted from 3 to 255. It is a key and not an option because this
  repository keeps operational settings in the configuration file.
- Two variables are added to the program's environment:
  `MAELYS_EGRESS_CHANNEL_FD` (the number) and
  `MAELYS_EGRESS_CHANNEL_CONNECT_TIMEOUT_MS` (the value of
  `channel_connect_timeout_ms`, which a client needs to bound its wait, as
  bootstrap v1 transmits it). A value already present under either name is
  replaced.
- The rest of the environment is inherited whole, as for delegates. That
  includes `MAELYS_CLI_FORMAT`: a program that is itself a Maelys CLI
  behaves as its caller asked.

A new program should read the variable; 4 is the default so that a program
written for Warden runs unchanged. `examples/channel_client.c` takes the
number from `argv[1]` today and learns the variable in the same change, so
that the repository shows two conventions and not three.

### 4. Configuration: a third mode

The catalog knows two modes and tells them apart by `channel_listen_unix`.
The third is named by what it has and lacks:

| Mode | `channel_principal` | `channel_listen_unix` | Command |
|---|---|---|---|
| proxy | absent | absent | `serve` |
| broker | present | present | `channel broker` |
| exec | present | absent | `channel exec` |

In exec mode: `channel_invocation_id`, `channel_connect_timeout_ms`,
`channel_fd`, the principal quotas, the admin listener, the audit keys and
the destinations are accepted. `channel_handshake_timeout_ms` and
`channel_max_clients` belong to the broker and are refused, as are every
proxy listener, credential and TLS listener key. `channel_fd` is refused
outside exec mode. Each command refuses a file of another mode with the
existing `VALIDATION_FAILED` message.

One visible change follows and goes to the changelog: a file with
`channel_principal` and no `channel_listen_unix` is refused by
`config validate` today and becomes valid. The cross-key constraint is
hard-coded in four places that move together: `cli/commands.c`
(`command_run`), `cli/config_catalog.c` (requirements and constraint text),
`cli/config_file.c` (`check_constraints`) and `cli/serve.c` (`native_only`
derived from `channel_listen_unix`).

### 5. Signals

The existing signal thread (`cli/reload.c`) stops the server on `SIGINT` and
`SIGTERM`. Stopping the server closes every relayed connection: reused as
is, a `SIGTERM` to the launcher would cut the program's network before the
program had seen any signal, and no graceful exit would be possible. In exec
mode the thread does this instead:

- `SIGINT` and `SIGTERM` are **forwarded to the program**, and nothing else
  happens. The server stops when the program has exited.
- `SIGHUP` remains the policy reload, as for the two other commands.
- The program stays in the launcher's process group and keeps its
  controlling terminal. An interactive program therefore works, and a
  `Ctrl-C` reaches it twice: once from the terminal, once forwarded. This is
  documented and not avoided, since detaching the program with `setsid`
  would break job control and macOS gives no way to tell a terminal signal
  from one sent to the launcher alone.
- No timer and no `SIGKILL`: a program that ignores `SIGTERM` keeps the
  launcher waiting. Killing the group is the supervisor's decision.

Two limits are stated, not solved:

- If the launcher is killed with `SIGKILL`, the program survives it with a
  dead channel: its relayed connections end, and its next request fails.
  Linux's parent-death signal is not used: it fires when the *thread* that
  started the program ends, and here that is the coordinator thread, so it
  would kill a program whose launcher is alive.
- A process the program starts inherits the descriptor unless the program
  sets `FD_CLOEXEC` on it, which a channel-aware program should do on
  entry. The launcher waits for the program only; what it leaves behind
  loses its connections when the server stops.

### 6. Order of operations

The main thread runs the server reactor; a coordinator thread does the
rest, on the model of `cli/channel_broker.c`: destroying the channel joins
its thread, and a request in flight completes on the server's reactor, so
the destruction cannot run on the thread that owns that reactor.

1. Load and check the configuration; create policy, configuration, server.
   Block `SIGINT`, `SIGTERM` and `SIGHUP` before any thread exists.
2. Coordinator: wait for the server to run, create the native connector,
   then the channel. The client end is `CLOEXEC` in the launcher.
3. Start the program with the client end on `channel_fd`. On failure: stop
   the server, report the envelope, exit 1.
4. Close the launcher's copy of the client end, start the signal thread,
   wait for the program.
5. When it has exited: **stop the server, then destroy the channel**, then
   release the connector, then destroy the server. This is the order
   `channel broker` uses. The reverse would wait for a request in flight, up
   to `channel_connect_timeout_ms`, with the reactor still running; stopping
   first cancels pending opens, and destroying the server closes the
   connections it still relays.
6. Exit with the program's status.

If the server stops by itself while the program runs, the launcher writes
the reason on stderr, ends the program's connections by destroying the
server, does not signal the program, waits for it and returns its status.

## What maelys-cli must provide

This repository forbids a hand-written `fork` and `execve`; external programs
start through `maelys_cli_process_*`. The pinned framework (0.5.30, also the
latest) has `maelys_cli_process_run`, which blocks until the program ends,
and `maelys_cli_process_replace`. Both pass descriptors 0, 1 and 2 and close
everything else at `exec`. Neither can pass the channel, and a blocking
`run` leaves no place to forward a signal from.

The forwarding belongs to this product's signal thread, which also owns the
reload; it does not belong inside a blocking call of the framework. The
request is therefore a launch separated from the wait, with a signal
operation, and a list of descriptors to inherit:

```c
typedef struct maelys_cli_process_inherit {
    int source;  /* a descriptor of the caller; may be CLOEXEC */
    int target;  /* its number in the program, >= 3; CLOEXEC cleared there */
} maelys_cli_process_inherit_t;

typedef struct maelys_cli_process_options {
    const maelys_cli_process_inherit_t *inherit;
    size_t inherit_count;  /* targets are distinct */
} maelys_cli_process_options_t;

/* Same trust checks as run. Returns once exec has succeeded, or -1 with
 * errno after reaping a child whose exec failed. */
int maelys_cli_process_start(const char *path, char *const argv[],
    char *const envp[], const maelys_cli_process_options_t *options,
    maelys_cli_process_t *out_process);
/* May be called from another thread than wait, at any time. Once the
 * program has been reaped it sends nothing and returns -1 with ESRCH. */
int maelys_cli_process_signal(maelys_cli_process_t *process, int signal_number);
int maelys_cli_process_wait(maelys_cli_process_t *process,
    maelys_cli_process_status_t *out_status);
```

Two requirements go with that signature.

**The inheritance is a mapping, applied as a whole.** `3 → 4` together with
`4 → 3` must work, and so must one source given to several targets:
installing the targets one after the other would overwrite a source that a
later entry still needs. Moving every source above the highest target
first, then installing each target, covers both and the case of a source
equal to its target. Two entries with the same target are refused before
anything starts.

**`signal` and `wait` share the handle between two threads.** Here the
signal thread forwards while the coordinator waits. Between the moment the
kernel gives up the program's identifier and the moment the handle records
it, a forwarded signal could reach an unrelated process that received the
same identifier. The contract must exclude that: after the program has been
reaped, `signal` sends nothing. Waiting without reaping (`waitid` with
`WNOWAIT`), then reaping under the lock `signal` takes, is one way to hold
it. A second `wait` on the same handle returns the recorded status.

Three more cases are acceptance criteria of that request, because each one
makes the program start without its channel and without an error:

1. **Target equal to source.** `dup2(4, 4)` does nothing and leaves
   `CLOEXEC` set; the kernel may well have given the client end the number
   4. The flag must be cleared explicitly.
2. **Target equal to a descriptor the child still needs**: the error pipe,
   the held executable, its held directory. They must be moved above the
   highest target before any `dup2`.
3. **Order.** Everything is marked `CLOEXEC` first, targets are installed
   after, so that the fallback loop used where `close_range` is missing does
   not mark a target again.

A target already open in the launcher is not a problem: it is `CLOEXEC` in
the child and replaced. A target below 3 is refused.

Also asked, as a convenience: a way to build an environment from the
caller's own plus an overlay, since `maelys_cli_environment_to_envp`
renders the overlay alone today.

## Tests the implementation carries

- Status: the program exits 0, 1, 2, 42; is killed by `SIGTERM` (143).
  A program that exits 0 while cleanup is made to fail is still reported 0.
- Not started: relative program path, untrusted program, missing program,
  wrong configuration mode, each with an envelope on stderr and exit 1, and
  nothing on stdout.
- Descriptors: in the program, `channel_fd` is open, is a datagram socket
  and answers a request; no other descriptor at or above 3 is open; the
  source-equals-target case is forced. Permutations and duplicate targets
  are the framework's tests, not this command's: it inherits one descriptor.
- Signals: `SIGTERM` to the launcher while the program holds a relayed
  connection — the program receives it, the connection stays usable until
  the program exits, then the upstream sees the end. `SIGHUP` reloads the
  policy and does not reach the program through the launcher. A signal
  that arrives once the program has exited is not forwarded.
- Shutdown: connections still open when the program exits are revoked; the
  launcher returns without waiting for `channel_connect_timeout_ms`.
- Contract: `describe channel.exec` has no `protocol`, the standard
  `exitCodes`, the operand and its rest; `make conformance-check` passes;
  `config validate` accepts and refuses each key in each of the three modes.
- A program that takes `--options` of its own after `--`, and an empty
  argument, which the framework's operand parser may refuse today.

## Order of work

1. This proposal, merged without a tag.
2. The request to maelys-cli, with the signature, the two requirements and
   the three cases. Advancing the pin is a product decision recorded in the
   changelog.
3. Meanwhile, without the framework, only what no user can observe: a
   receipt sink that can be left out, and the exec mode of the signal
   thread. The third configuration mode does **not** come early:
   `config validate` would accept a file that no command can run, and a
   patch release may leave before the command exists.
4. In one change, once the pin carries the new process functions: the three
   configuration modes with `config validate` and the generated reference,
   the `channel.exec` catalog entry, its handler and the tests.
5. `examples/channel_client.c` reads the variable; one line in
   `examples/README.md` runs it through `channel exec`. Then a release: the
   CLI, its reference and the configuration contract change.

## Questions closed on review

1. **No call in `libmaelys_egress_client` that reads the environment**, in
   this version. The example shows how to read and check the descriptor and
   the timeout; the client interface does not grow before the use is known.
2. **No receipt file distinct from `audit_log`.** Stdout stays the
   program's alone.
3. **`channel_fd` is a configuration key**, default 4. It follows the
   repository's convention and leaves one way to set the number.

## What the implementation changed

1. **The connections end when the server is destroyed, not when it stops.**
   The order of section 6 holds, with one more step that the proposal took
   for granted: stopping cancels the opens in flight, and it is
   `maelys_egress_server_destroy` that closes the connections still relayed.
   The launcher therefore destroys the server itself, and does so at once
   when the server stops while the program runs: waiting for the program
   first would leave its connections open and silent, with nothing to read
   and no end.
2. **The framework's handle is opaque and is released.** maelys-cli kept the
   requested `start`, `signal` and `wait`, added
   `maelys_cli_process_release`, and holds the guarantee without a lock:
   `wait` observes the exit without reaping, so the process identifier stays
   reserved until the release, and `signal` after `wait` sends nothing. The
   signature above is the request as sent, not the one that exists.
3. **An empty argument is accepted.** The test list feared that the operand
   parser would refuse `""`; it does not for an untyped operand, and the
   test holds that the program receives it.
4. **The mutants.** `exec-signal-stops-server`, `exec-receipt-on-stdout`,
   `exec-status-replaced` and `exec-broker-keys-accepted` each undo one
   decision of this document and are killed by an assertion of the test.
