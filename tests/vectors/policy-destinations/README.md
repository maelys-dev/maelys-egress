# Network destination corpus

The normative cases of the network destination contract
(`docs/destination-contract.md`, contract 1). `VERSION` is the corpus
version; it moves when a case is added, removed or changes its expectation.

The corpus works on **sealed destinations and single requests**, the level
a mediator receives. It needs no network: what a name resolves to is stated
by the request line, and every name is already canonical.

## Contract

In `mediated` mode, a request is allowed when at least one destination
matches it (protocol, port, name equal or under a `*.suffix` wildcard) and
admits it under its own flags: `requireTlsSni` demands one DNS server name
equal to the request name, and a private address demands
`allowPrivateAddresses`. Grants are additive and order never matters. `none`
refuses everything; `direct` allows everything, well formed or not, since no
mediator judges it. A strict IPv4 literal (four decimal octets, no leading
zero) is a name like any other: an exact destination may name one, a
wildcard never covers one. A name whose last label is numeric (digits, or
`0x` and hex digits) and which is not a strict literal is refused at the
source and, as a request, `malformed-name`.

## Case format

One case per `cases/*.case` file, UTF-8, one directive per line; `#` starts
a comment. Names contain no whitespace.

```
network <none|direct|mediated>
destination tcp <name|*.suffix> <port> [require-tls-sni] [allow-private-addresses]
requires <capability>
request tcp <name> <port> <sni=<name>|sni=absent> [private] <allowed|refused <reason>>
compile <accepted|refused>
```

- `network` is `mediated` when absent. `destination` lines exist only in
  `mediated` mode.
- `private` on a request states that the name resolves, at the time of the
  request, to at least one private address. Without it, every address is
  global. The request line carries that fact so that no consumer resolves
  anything to play the corpus.
- `requires network-host-wildcard` states that the policy needs that
  capability, which it does exactly when a destination is a wildcard. A
  mediator that does not announce it refuses the policy before launch.
- `request` lines state what the contract decides. They hold for every
  order of the destinations. A refusal names its reason with the identifier
  of the contract (`network-none`, `malformed-name`, `no-destination`,
  `sni-absent`, `sni-mismatch`, `private-address`).
- `compile accepted`: the policy is well formed under the contract.
- `compile refused`: the policy is refused at the source, before any
  request: an exact name whose last label is numeric without being a strict
  IPv4 literal, an IPv4 literal carrying `require-tls-sni` (RFC 6066 admits
  no literal in a server name), a wildcard whose suffix has fewer than two labels or whose
  last label is numeric, a wildcard written in any other form than
  `*.suffix`, or a wildcard carrying `allow-private-addresses`. A refused case has no `request` line.

Until MIR v3 carries the wildcard form, `maelys_mir` refuses every
destination whose name starts with `*`: the cases marked `requires
network-host-wildcard` are refused at the source today, and the cases
marked `compile refused` with them. The runner of this repository checks
exactly that, so that adding the form to the format is what turns them on.

## Conformance of a mediator

A mediator runs every `request` of the accepted cases it can seal, with the
stated server name and the stated resolution, against what it enforces. A
result is conformant when the verdict is **enforced as stated** — the
connection is made exactly when the verdict is `allowed` — or when the
policy is **refused before launch** because the mediator cannot guarantee
it. A refusal is not an enforced case and is reported apart.

A mediator that resolves exact names when it seals the policy may refuse,
before launch, a policy whose exact name resolves to a private address
without `allow-private-addresses`: the request marked `private` is then
never made, which is conformant.

### What a mediator proves for a wildcard

Answering the requests is not enough for a wildcard: its names are resolved
at the request, not at sealing. For each wildcard destination, a mediator
that enforces the case also proves, in an execution under the policy:

| Obligation | What the mediator proves |
|---|---|
| `resolve-at-request` | the request name, and no other, is resolved when the request is made, and the connection goes to an address that resolution returned |
| `private-per-request` | every address of that resolution is checked; one private address refuses the request |
| `receipt-address` | the receipt of the connection carries the address actually contacted |

The identifiers are stable: a report names an obligation by them. A
mediator that cannot hold all three does not announce
`network-host-wildcard`.

**Known limit.** A wildcard on a suffix shared by many tenants admits every
tenant: the corpus refuses one-label suffixes and nothing more. The limit is
the contract's, not the mediator's, and a report does not count it as a gap.
