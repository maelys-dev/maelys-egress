# TLS deployment

## Which connection does TLS protect?

There can be two independent TLS connections:

```text
client ══ optional listener TLS ══► Egress ── encrypted origin tunnel ──► website
          Mbed TLS or wolfSSL                    application TLS
```

- **Listener TLS** protects the client-to-Egress hop. It is required when the
  proxy listener is reachable beyond numeric loopback because proxy
  credentials otherwise cross that network in plaintext.
- **Origin TLS** belongs to the application and destination. For an HTTPS URL,
  the application sends HTTP `CONNECT`, then performs its own TLS handshake
  through the admitted tunnel. Egress does not decrypt that connection.

Adding Mbed TLS or wolfSSL therefore does **not** turn Egress into a TLS MITM.

## Mbed TLS or wolfSSL?

Both providers implement the same Egress TLS ABI and expose identical CLI
options. Choose one at build/deployment time:

| Provider | When it is a natural choice |
|---|---|
| Mbed TLS | Default recommendation for a modular C deployment and permissive distribution |
| wolfSSL | Existing wolfSSL estate or an aggressively embedded deployment |

The Egress policy, proxy parsers, receipts and reactor do not depend on that
choice. There is no automatic fallback: a binary named `maelys-egress-mbedtls`
uses Mbed TLS, and `maelys-egress-wolfssl` uses wolfSSL. Review the selected
library's license and build options for your distribution.

## Build the two optional binaries

Ubuntu 24.04:

```sh
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libmbedtls-dev libwolfssl-dev
make tls-providers-check
make tls-binaries
```

macOS with Homebrew:

```sh
brew install mbedtls wolfssl pkg-config
make tls-providers-check
make tls-binaries
```

The commands produce:

```text
build/release/bin/maelys-egress-mbedtls
build/release/bin/maelys-egress-wolfssl
```

The ordinary `maelys-egress` binary intentionally contains neither TLS stack and
does not accept `tls_*` configuration keys.

## Runnable local TLS-listener example

The following certificate is for development only. It demonstrates the
mechanics without disabling certificate verification.

```sh
install -d -m 0700 "$HOME/.maelys-egress-tls-demo"
cd "$HOME/.maelys-egress-tls-demo"

openssl req -x509 -newkey rsa:2048 -nodes -days 30 \
  -keyout proxy-key.pem -out proxy-cert.pem \
  -subj '/CN=maelys-egress-local' \
  -addext 'subjectAltName=IP:127.0.0.1'
chmod 0600 proxy-key.pem

openssl rand -base64 32 > token
chmod 0600 token

cat > egress.conf <<EOF
schema_version = 1
listen = 127.0.0.1:18443
token_file = $PWD/token
tls_cert = $PWD/proxy-cert.pem
tls_key = $PWD/proxy-key.pem
allow_tls_sni = github.com:443
EOF
chmod 0600 egress.conf
```

Validate with the provider-specific binary and start it in terminal A:

```sh
/path/to/maelys-egress-mbedtls config validate --config "$PWD/egress.conf"
/path/to/maelys-egress-mbedtls serve --config "$PWD/egress.conf"
```

Use `maelys-egress-wolfssl` in both commands to test the other provider; the
configuration is unchanged.

In terminal B, tell curl that the **proxy URL itself** is HTTPS and trust the
demo certificate for that proxy connection:

```sh
TOKEN=$(cat "$HOME/.maelys-egress-tls-demo/token")

curl --proxy https://127.0.0.1:18443 \
  --proxy-cacert "$HOME/.maelys-egress-tls-demo/proxy-cert.pem" \
  --proxy-user "maelys:${TOKEN}" \
  https://github.com/
```

`--proxy-cacert` verifies Egress's listener certificate. It is distinct from
curl's normal CA store, which still verifies `github.com`. Avoid
`--proxy-insecure` outside a disposable diagnostic because it removes the
listener identity check.

For a remote deployment, replace the self-signed certificate with one issued
for the DNS name clients use, bind the intended non-loopback numeric address,
and restrict that port with the host firewall.

## Add mutual TLS

Normal proxy authentication answers “which Egress principal is this request?”
Mutual TLS additionally answers “which machine or workload certificate reached
the listener?” Both checks apply; mTLS does not replace the token.

Create a demonstration client CA and client certificate:

```sh
cd "$HOME/.maelys-egress-tls-demo"

openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout client-ca-key.pem -out client-ca.pem \
  -subj '/CN=maelys-egress-demo-client-ca'
openssl req -newkey rsa:2048 -nodes \
  -keyout client-key.pem -out client.csr \
  -subj '/CN=demo-workload'
openssl x509 -req -in client.csr \
  -CA client-ca.pem -CAkey client-ca-key.pem -CAcreateserial \
  -out client.pem -days 30 -sha256
chmod 0600 client-ca-key.pem client-key.pem
```

Add to `egress.conf` and restart:

```text
tls_ca = /absolute/path/client-ca.pem
require_client_cert = true
```

Then supply the client identity:

```sh
curl --proxy https://127.0.0.1:18443 \
  --proxy-cacert "$HOME/.maelys-egress-tls-demo/proxy-cert.pem" \
  --proxy-cert "$HOME/.maelys-egress-tls-demo/client.pem" \
  --proxy-key "$HOME/.maelys-egress-tls-demo/client-key.pem" \
  --proxy-user "maelys:${TOKEN}" \
  https://github.com/
```

A missing, expired or untrusted client certificate fails during listener TLS;
a wrong proxy token fails afterward as proxy authentication.

## Embedding a TLS provider in C

Applications linking the optional module construct a provider explicitly:

```c
#include <maelys/egress.h>
#include <maelys/egress_tls_modules.h>

maelys_egress_tls_files_t files = {
    .certificate_file = "proxy-cert.pem",
    .private_key_file = "proxy-key.pem",
    .ca_file = NULL,
    .require_client_certificate = 0
};
maelys_egress_tls_provider_t *provider = NULL;

maelys_egress_tls_mbedtls_create(&files, &provider, &error);
maelys_egress_config_set_tls_listener(config, provider, &error);
maelys_egress_tls_provider_release(provider);
```

Replace the constructor with `maelys_egress_tls_wolfssl_create` without changing
the server/configuration lifecycle. The configuration retains the provider.
Production code must check every result and release the owned error; the
fragment only shows provider selection.

## Why this is not MITM

TLS interception would additionally require a protected local CA, dynamic leaf
certificate generation, trust-root installation/removal, upstream certificate
verification, decrypted-content policy and explicit behavior for certificate
pinning, client certificates and ECH. None of that is implied by listener TLS,
and Egress 0.7 does not implement it.
