# C examples

- `basic_proxy.c` is a complete embedded server with authentication, exact SNI
  policy, receipts and signal-safe shutdown through a waiter thread.
- `native_connector.c` opens a policy-checked, Egress-relayed TCP stream without
  making the embedding application speak HTTP proxy or SOCKS5. The public
  ownership, authentication and deadline contract is declared in
  [`include/maelys/egress.h`](../include/maelys/egress.h).
- `policy_reload.c` replaces a sealed policy from a control thread while the
  owner thread runs the reactor.
- `metrics_snapshot.c` reads the immutable aggregate snapshot API.
- `durable_audit.c` opens or verifies an HMAC-chained journal. Its literal key
  is intentionally unsuitable for production.
- `custom_attestor.c` shows the provider ABI with a deliberately non-secure
  demonstration callback; replace it with a real signer.
- `channel_supervisor.c` and `channel_client.c` are the two sides of the
  [mediated-connection channel](../protocol/egress-channel-v1.md): the
  supervisor runs Egress with no port, binds a channel to a connector and
  hands its client end to a child on a descriptor number of its choosing; the
  child links `libmaelys_egress_client` alone and asks for one destination.
  Without arguments the supervisor serves a loopback echo, so the round trip
  needs no network.

Run `make examples-check`. Installed examples live under
`share/doc/maelys-egress/examples/c`.
