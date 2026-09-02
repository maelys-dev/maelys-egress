# Security model

## Guarantees

- Policy is deny-by-default and immutable after sealing.
- A requested host and port must match an exact allowlist entry before a socket
  is created.
- Each allowlist hostname is resolved once and its concrete addresses are
  pinned for the sealed policy lifetime. The sorted pinned address set is part
  of the policy digest, so a receipt identifies the enforced endpoints rather
  than only the original DNS names.
- Any non-global, loopback, link-local, private, documentation, multicast or
  unspecified address causes sealing to fail unless that destination opted in.
- Plaintext proxy listeners are loopback-only. A remote listener requires an
  explicitly configured TLS provider and proxy authentication.
- Credentials are compared without data-dependent early exit, stripped from
  forwarded HTTP, omitted from receipts and cleared from owned configuration
  storage at destruction.
- Principal secrets are unique and map to canonical invocation IDs. Receipts
  contain the invocation ID but never the username, secret or proxy URL.
- Buffers, headers, destinations, connections and pinned address counts are
  bounded.
- Active-stream, per-stream payload and execution-cumulative payload quotas are
  independent. Byte I/O is shortened to the exact remaining allowance before
  each system call, and the owner reactor serializes cumulative accounting
  across concurrent streams.
- System loop watch identities and Egress connection generations defend against
  descriptor/slot reuse.
- Destinations configured with the TLS identity guard forward no tunnel bytes
  until one bounded ClientHello names exactly the sealed destination. Missing,
  duplicate, malformed and mismatched SNI fail closed.

## Residual risks and non-guarantees

- Egress is not a sandbox. A process with ambient network access can bypass it.
- Proxy environment produced by `maelys_egress_profile` is configuration, not an
  enforcement boundary. An Executor adapter must independently prove exclusive
  proxy reachability for its selected backend.
- Policy sealing uses the platform `getaddrinfo` resolver synchronously. The
  operating system controls its startup timeout. Runtime request processing
  performs no DNS lookup.
- DNS addresses remain pinned for one immutable policy generation. ABI 2 offers
  `policy_reseal` to resolve a fresh generation, but intentionally does not
  infer or schedule DNS TTLs. Operators should reseal periodically and swap
  only after the fresh policy succeeds.
- An unguarded `CONNECT` remains an opaque TCP tunnel. The optional guard proves
  only the cleartext ClientHello SNI, not the peer certificate or application
  protocol. ECH is refused by guarded destinations because a passive relay
  cannot observe inner SNI. For controlled or explicitly cooperating origins,
  a future RFC 9849 split client-facing service could decrypt only the inner
  ClientHello and forward the remaining TLS exchange opaque; Egress does not ship
  that mode. Exact peer-certificate verification or content inspection would
  still require application-TLS termination or explicit backend cooperation.
- HTTP Basic/Bearer and SOCKS username/password are cleartext on a plaintext
  loopback listener. Remote listeners are admitted only behind the configured
  TLS provider; mutual TLS can additionally authenticate the transport peer.
- TLS private keys and CA files are parsed by the selected provider. File
  ownership, rotation and hardware-backed key isolation remain operational
  responsibilities; the file-backed reference constructors do not mint keys.
- No TLS interception, content inspection, malware scanning, certificate
  generation or credential injection is claimed.
- A callback-only receipt is a process-local observation. The optional durable
  journal authenticates its exact canonical representation with a chained
  HMAC-SHA-256, which is verifiable by holders of the shared key but is not an
  independent public-key attestation. The optional attestor seam can attach an
  asymmetric or hardware-backed signature; Egress deliberately does not choose
  or bundle a signing algorithm in its minimal binary. Complete tail deletion
  still requires an external checkpoint of the final chain value to detect.

## Fail-closed cases

The server refuses: an unsealed/empty policy, unpinned resolution, private
resolution without explicit permission, plaintext non-loopback listeners, missing
authentication without the development override, duplicated destinations,
unknown destinations, authority/Host mismatch, ambiguous request lengths,
transfer-encoded HTTP forward requests, failed TLS configuration/handshake,
unsupported SOCKS commands and every backend ABI mismatch.
