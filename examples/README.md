# C examples

- `basic_proxy.c` is a complete embedded server with authentication, exact SNI
  policy, receipts and signal-safe shutdown through a waiter thread.
- `native_connector.c` opens a policy-checked, Egress-relayed TCP stream without
  making the embedding application speak HTTP proxy or SOCKS5. The complete
  ownership, authentication and deadline contract is in
  `docs/native-connector.md`.
- `policy_reload.c` replaces a sealed policy from a control thread while the
  owner thread runs the reactor.
- `metrics_snapshot.c` reads the immutable aggregate snapshot API.
- `durable_audit.c` opens or verifies an HMAC-chained journal. Its literal key
  is intentionally unsuitable for production.
- `custom_attestor.c` shows the provider ABI with a deliberately non-secure
  demonstration callback; replace it with a real signer.

Run `make examples-check`. Installed examples live under
`share/doc/maelys-egress/examples/c`.
