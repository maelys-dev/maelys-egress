#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "maelys/egress.h"
#include "maelys/egress_tls.h"
#include "src/internal.h"
#include "maelys/egress_profile.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static maelys_egress_result_t fake_session_create(
    void *context, maelys_egress_tls_role_t role, int fd,
    const char *server_name, void **out_session, char **out_error) {
    (void)role; (void)fd; (void)server_name; (void)out_error;
    *out_session = context;
    return MAELYS_EGRESS_OK;
}
static maelys_egress_tls_step_t fake_handshake(void *context, void *session) {
    return context == session ? MAELYS_EGRESS_TLS_COMPLETE : MAELYS_EGRESS_TLS_FAILED;
}
static maelys_egress_tls_step_t fake_read(
    void *context, void *session, void *buffer, size_t capacity, size_t *out_read) {
    (void)context; (void)session; (void)buffer; (void)capacity; *out_read = 0u;
    return MAELYS_EGRESS_TLS_WANT_READ;
}
static maelys_egress_tls_step_t fake_write(
    void *context, void *session, const void *buffer, size_t length, size_t *out_written) {
    (void)context; (void)session; (void)buffer; *out_written = length;
    return MAELYS_EGRESS_TLS_COMPLETE;
}
static maelys_egress_tls_step_t fake_shutdown(void *context, void *session) {
    (void)context; (void)session; return MAELYS_EGRESS_TLS_CLOSED;
}
static const char *fake_last_error(void *context, const void *session) {
    (void)context; (void)session; return "fake TLS error";
}
static void fake_release(void *context, void *session) { (void)context; (void)session; }
static int released_contexts;
static void fake_context_release(void *context) {
    CHECK(context != NULL);
    ++released_contexts;
}

typedef struct passthrough_session { int fd; } passthrough_session_t;
static maelys_egress_result_t passthrough_create(
    void *context, maelys_egress_tls_role_t role, int fd,
    const char *server_name, void **out_session, char **out_error) {
    (void)context; (void)role; (void)server_name; (void)out_error;
    passthrough_session_t *session = malloc(sizeof(*session));
    if (!session) return MAELYS_EGRESS_ERR_MEMORY;
    session->fd = fd;
    *out_session = session;
    return MAELYS_EGRESS_OK;
}
static maelys_egress_tls_step_t passthrough_handshake(void *context, void *session) {
    (void)context; (void)session; return MAELYS_EGRESS_TLS_COMPLETE;
}
static maelys_egress_tls_step_t passthrough_read(
    void *context, void *opaque, void *buffer, size_t capacity, size_t *out_read) {
    (void)context;
    passthrough_session_t *session = opaque;
    ssize_t count = recv(session->fd, buffer, capacity, 0);
    *out_read = count > 0 ? (size_t)count : 0u;
    if (count > 0) return MAELYS_EGRESS_TLS_COMPLETE;
    if (count == 0) return MAELYS_EGRESS_TLS_CLOSED;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ?
        MAELYS_EGRESS_TLS_WANT_READ : MAELYS_EGRESS_TLS_FAILED;
}
static maelys_egress_tls_step_t passthrough_write(
    void *context, void *opaque, const void *buffer, size_t length,
    size_t *out_written) {
    (void)context;
    passthrough_session_t *session = opaque;
    ssize_t count = send(session->fd, buffer, length, 0);
    *out_written = count > 0 ? (size_t)count : 0u;
    if (count > 0) return MAELYS_EGRESS_TLS_COMPLETE;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ?
        MAELYS_EGRESS_TLS_WANT_WRITE : MAELYS_EGRESS_TLS_FAILED;
}
static maelys_egress_tls_step_t passthrough_shutdown(void *context, void *opaque) {
    (void)context;
    passthrough_session_t *session = opaque;
    return shutdown(session->fd, SHUT_WR) == 0 || errno == ENOTCONN ?
        MAELYS_EGRESS_TLS_COMPLETE : MAELYS_EGRESS_TLS_FAILED;
}
static const char *passthrough_error(void *context, const void *session) {
    (void)context; (void)session; return "passthrough transport error";
}
static void passthrough_release(void *context, void *session) {
    (void)context; free(session);
}
static maelys_egress_tls_provider_t *make_passthrough_provider(void) {
    const maelys_egress_tls_ops_t ops = {
        .abi_version = MAELYS_EGRESS_TLS_ABI_VERSION,
        .name = "test-passthrough",
        .session_create = passthrough_create,
        .handshake = passthrough_handshake,
        .read = passthrough_read,
        .write = passthrough_write,
        .shutdown = passthrough_shutdown,
        .last_error = passthrough_error,
        .session_release = passthrough_release
    };
    maelys_egress_tls_provider_t *provider = NULL;
    CHECK(maelys_egress_tls_provider_create(
        &ops, NULL, NULL, &provider, NULL) == MAELYS_EGRESS_OK);
    return provider;
}

static void test_version_and_tls_seam(void) {
    CHECK(strcmp(maelys_egress_version_string(), MAELYS_EGRESS_BUILD_VERSION) == 0);
    CHECK(maelys_egress_abi_version() == 2u);
    CHECK(strcmp(maelys_egress_result_string(MAELYS_EGRESS_ERR_CRYPTO), "crypto") == 0);
    maelys_egress_tls_ops_t bad = {.abi_version = 2u, .name = "bad"};
    maelys_egress_tls_provider_t *provider = NULL;
    char *error = NULL;
    CHECK(maelys_egress_tls_provider_create(&bad, NULL, NULL, &provider, &error) ==
          MAELYS_EGRESS_ERR_UNSUPPORTED);
    CHECK(error && strstr(error, "ABI 2") && strstr(error, "ABI 1"));
    maelys_egress_error_free(error);
    char provider_name[] = "fake";
    maelys_egress_tls_ops_t ops = {
        .abi_version = MAELYS_EGRESS_TLS_ABI_VERSION, .name = provider_name,
        .session_create = fake_session_create, .handshake = fake_handshake,
        .read = fake_read, .write = fake_write, .shutdown = fake_shutdown,
        .last_error = fake_last_error,
        .session_release = fake_release
    };
    error = NULL;
    CHECK(maelys_egress_tls_provider_create(
        &ops, &ops, fake_context_release, &provider, &error) ==
          MAELYS_EGRESS_OK);
    provider_name[0] = 'X';
    CHECK(strcmp(maelys_egress_tls_provider_name(provider), "fake") == 0);
    void *session = NULL;
    CHECK(provider->ops.session_create(provider->context, MAELYS_EGRESS_TLS_CLIENT,
        -1, "example.com", &session, &error) == MAELYS_EGRESS_OK);
    CHECK(provider->ops.handshake(provider->context, session) ==
          MAELYS_EGRESS_TLS_COMPLETE);
    CHECK(strcmp(provider->ops.last_error(provider->context, session),
                 "fake TLS error") == 0);
    provider->ops.session_release(provider->context, session);
    maelys_egress_tls_provider_retain(provider);
    maelys_egress_tls_provider_release(provider);
    maelys_egress_tls_provider_release(provider);
    CHECK(released_contexts == 1);
    maelys_egress_error_free(error);
}

static void test_policy_fail_closed(void) {
    maelys_egress_policy_t *policy = NULL;
    char *error = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "LOCALHOST", 443u, 0, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_ERR_DENIED);
    CHECK(error != NULL);
    maelys_egress_error_free(error);
    maelys_egress_policy_destroy(policy);

    policy = NULL; error = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "127.0.0.1", 443u, 1, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_is_sealed(policy));
    CHECK(strlen(maelys_egress_policy_digest_hex(policy)) == 64u);
    CHECK(strcmp(maelys_egress_policy_digest_hex(policy),
        "f2e7f6d083b29635d594245e2d0b9f244f7bc79c7c4b8e56d47b56d545b93524") == 0);
    CHECK(maelys_egress_policy_allow_tcp(policy, "example.com", 443u, 0, &error) ==
          MAELYS_EGRESS_ERR_ARGUMENT);
    maelys_egress_error_free(error); error = NULL;
    maelys_egress_policy_t *fresh = NULL;
    CHECK(maelys_egress_policy_reseal(policy, &fresh, &error) == MAELYS_EGRESS_OK);
    CHECK(fresh != policy && maelys_egress_policy_is_sealed(fresh));
    CHECK(strcmp(maelys_egress_policy_digest_hex(policy),
                 maelys_egress_policy_digest_hex(fresh)) == 0);
    maelys_egress_policy_destroy(fresh);
    maelys_egress_error_free(error);
    maelys_egress_policy_destroy(policy);
}

static size_t make_client_hello(
    const char *host, unsigned char output[EGRESS_HANDSHAKE_MAX]) {
    size_t host_length = strlen(host);
    size_t extension_length = 9u + host_length;
    size_t body_length = 43u + extension_length;
    size_t handshake_length = 4u + body_length;
    size_t used = 0u;
    output[used++] = 22u; output[used++] = 3u; output[used++] = 3u;
    output[used++] = (unsigned char)(handshake_length >> 8u);
    output[used++] = (unsigned char)handshake_length;
    output[used++] = 1u;
    output[used++] = (unsigned char)(body_length >> 16u);
    output[used++] = (unsigned char)(body_length >> 8u);
    output[used++] = (unsigned char)body_length;
    output[used++] = 3u; output[used++] = 3u;
    memset(output + used, 0x5au, 32u); used += 32u;
    output[used++] = 0u;
    output[used++] = 0u; output[used++] = 2u;
    output[used++] = 0x13u; output[used++] = 0x01u;
    output[used++] = 1u; output[used++] = 0u;
    output[used++] = (unsigned char)(extension_length >> 8u);
    output[used++] = (unsigned char)extension_length;
    output[used++] = 0u; output[used++] = 0u;
    size_t sni_length = 5u + host_length;
    output[used++] = (unsigned char)(sni_length >> 8u);
    output[used++] = (unsigned char)sni_length;
    size_t list_length = 3u + host_length;
    output[used++] = (unsigned char)(list_length >> 8u);
    output[used++] = (unsigned char)list_length;
    output[used++] = 0u;
    output[used++] = (unsigned char)(host_length >> 8u);
    output[used++] = (unsigned char)host_length;
    memcpy(output + used, host, host_length); used += host_length;
    return used;
}

static void test_tls_client_hello_identity(void) {
    unsigned char hello[EGRESS_HANDSHAKE_MAX];
    size_t length = make_client_hello("example.com", hello);
    char *error = NULL;
    CHECK(egress_tls_client_hello_matches(hello, 4u, "example.com", &error) == 0);
    CHECK(error == NULL);
    CHECK(egress_tls_client_hello_matches(hello, length, "example.com", &error) == 1);
    CHECK(error == NULL);
    CHECK(egress_tls_client_hello_matches(hello, length, "other.example", &error) == -1);
    CHECK(error && strstr(error, "does not match"));
    maelys_egress_error_free(error); error = NULL;
    hello[0] = 23u;
    CHECK(egress_tls_client_hello_matches(hello, length, "example.com", &error) == -1);
    CHECK(error != NULL);
    maelys_egress_error_free(error);

    maelys_egress_policy_t *policy = NULL;
    error = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "example.com", 443u, 0, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_require_tls_sni(policy, "example.com", 443u, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_require_tls_sni(policy, "127.0.0.1", 443u, &error) ==
          MAELYS_EGRESS_ERR_ARGUMENT);
    maelys_egress_error_free(error);
    maelys_egress_policy_destroy(policy);

    char digest[65];
    egress_sha256_hex("", 0u, digest);
    CHECK(strcmp(digest,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    egress_sha256_hex("abc", 3u, digest);
    CHECK(strcmp(digest,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    unsigned char hmac_key[20];
    memset(hmac_key, 0x0b, sizeof(hmac_key));
    unsigned char hmac[32];
    static const unsigned char expected_hmac[32] = {
        0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,
        0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
        0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,
        0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7
    };
    CHECK(egress_hmac_sha256(hmac_key, sizeof(hmac_key),
        "Hi There", 8u, hmac));
    CHECK(memcmp(hmac, expected_hmac, sizeof(hmac)) == 0);
}

static void test_address_classification(void) {
    char canonical[EGRESS_MAX_HOST + 1u];
    const char non_ascii[] = {'e', (char)0xe9, '\0'};
    CHECK(!egress_canonical_host(non_ascii, canonical));
    static const char *private_v4[] = {
        "0.0.0.0", "10.0.0.1", "100.64.0.1", "127.0.0.1",
        "169.254.1.1", "172.16.0.1", "192.0.2.1", "192.168.1.1",
        "198.18.0.1", "198.51.100.1", "203.0.113.1", "224.0.0.1"
    };
    for (size_t i = 0; i < sizeof(private_v4) / sizeof(private_v4[0]); ++i) {
        struct sockaddr_in address = {.sin_family = AF_INET};
        CHECK(inet_pton(AF_INET, private_v4[i], &address.sin_addr) == 1);
        CHECK(egress_address_is_private((const struct sockaddr *)&address));
    }
    struct sockaddr_in public_v4 = {.sin_family = AF_INET};
    CHECK(inet_pton(AF_INET, "8.8.8.8", &public_v4.sin_addr) == 1);
    CHECK(!egress_address_is_private((const struct sockaddr *)&public_v4));
    static const char *private_v6[] = {
        "::", "::1", "::ffff:127.0.0.1", "100::1", "2001:db8::1",
        "2002::1", "3fff::1", "5f00::1", "fc00::1", "fe80::1", "ff02::1"
    };
    for (size_t i = 0; i < sizeof(private_v6) / sizeof(private_v6[0]); ++i) {
        struct sockaddr_in6 address = {.sin6_family = AF_INET6};
        CHECK(inet_pton(AF_INET6, private_v6[i], &address.sin6_addr) == 1);
        CHECK(egress_address_is_private((const struct sockaddr *)&address));
    }
    struct sockaddr_in6 public_v6 = {.sin6_family = AF_INET6};
    CHECK(inet_pton(AF_INET6, "2606:4700:4700::1111", &public_v6.sin6_addr) == 1);
    CHECK(!egress_address_is_private((const struct sockaddr *)&public_v6));
}

static void test_http_parser_adversarial(void) {
    maelys_egress_config_t *config = NULL;
    char *error = NULL;
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_authentication(
        config, "maelys", "0123456789abcdef", &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_add_principal(config, "worker",
        "fedcba9876543210", "invocation-42", &error) == MAELYS_EGRESS_OK);
    static const char valid[] =
        "GET http://example.com/path HTTP/1.1\r\nHost: example.com\r\n"
        "Proxy-Authorization: Bearer 0123456789abcdef\r\nContent-Length: 0\r\n\r\n";
    egress_proxy_request_t request;
    memset(&request, 0, sizeof(request));
    CHECK(egress_parse_http_request((const unsigned char *)valid,
        sizeof(valid) - 1u, config, &request, &error) == 1);
    CHECK(request.protocol == MAELYS_EGRESS_PROTOCOL_HTTP_FORWARD);
    CHECK(strcmp(request.host, "example.com") == 0 && request.port == 80u);
    CHECK(request.forward_bytes != NULL);
    CHECK(strstr((char *)request.forward_bytes, "Proxy-Authorization") == NULL);
    egress_proxy_request_clear(&request);
    static const char correlated[] =
        "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n"
        "Proxy-Authorization: Bearer fedcba9876543210\r\n\r\n";
    CHECK(egress_parse_http_request((const unsigned char *)correlated,
        sizeof(correlated) - 1u, config, &request, &error) == 1);
    CHECK(strcmp(request.invocation_id, "invocation-42") == 0);
    egress_proxy_request_clear(&request);
    static const char basic[] =
        "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n"
        "Proxy-Authorization: Basic bWFlbHlzOjAxMjM0NTY3ODlhYmNkZWY=\r\n\r\n";
    CHECK(egress_parse_http_request((const unsigned char *)basic,
        sizeof(basic) - 1u, config, &request, &error) == 1);
    egress_proxy_request_clear(&request);
    static const char noncanonical_basic[] =
        "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n"
        "Proxy-Authorization: Basic bWFlbHlzOjAxMjM0NTY3ODlhYmNkZWZ=\r\n\r\n";
    CHECK(egress_parse_http_request((const unsigned char *)noncanonical_basic,
        sizeof(noncanonical_basic) - 1u, config, &request, &error) == -2);
    maelys_egress_error_free(error); error = NULL;

    static const char mismatch[] =
        "CONNECT example.com:443 HTTP/1.1\r\nHost: attacker.example:443\r\n"
        "Proxy-Authorization: Bearer 0123456789abcdef\r\n\r\n";
    CHECK(egress_parse_http_request((const unsigned char *)mismatch,
        sizeof(mismatch) - 1u, config, &request, &error) == -1);
    maelys_egress_error_free(error); error = NULL;
    static const char smuggle[] =
        "POST http://example.com/x HTTP/1.1\r\nHost: example.com\r\n"
        "Proxy-Authorization: Bearer 0123456789abcdef\r\n"
        "Content-Length: 1\r\nContent-Length: 2\r\n\r\n";
    CHECK(egress_parse_http_request((const unsigned char *)smuggle,
        sizeof(smuggle) - 1u, config, &request, &error) == -1);
    maelys_egress_error_free(error); error = NULL;
    static const char transfer[] =
        "POST http://example.com/x HTTP/1.1\r\nHost: example.com\r\n"
        "Proxy-Authorization: Bearer 0123456789abcdef\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    CHECK(egress_parse_http_request((const unsigned char *)transfer,
        sizeof(transfer) - 1u, config, &request, &error) == -1);
    maelys_egress_error_free(error); error = NULL;
    static const char wrong_secret[] =
        "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n"
        "Proxy-Authorization: Bearer 0000000000000000\r\n\r\n";
    CHECK(egress_parse_http_request((const unsigned char *)wrong_secret,
        sizeof(wrong_secret) - 1u, config, &request, &error) == -2);
    maelys_egress_error_free(error); error = NULL;
    static const char unbracketed_ipv6[] =
        "CONNECT 2001:db8::1 HTTP/1.1\r\nHost: 2001:db8::1\r\n"
        "Proxy-Authorization: Bearer 0123456789abcdef\r\n\r\n";
    CHECK(egress_parse_http_request((const unsigned char *)unbracketed_ipv6,
        sizeof(unbracketed_ipv6) - 1u, config, &request, &error) == -1);
    maelys_egress_error_free(error);
    maelys_egress_config_destroy(config);
}

static void test_execution_profile(void) {
    maelys_egress_profile_t *profile = NULL;
    char *error = NULL;
    CHECK(maelys_egress_profile_create("127.0.0.1", 8080u, "worker+name",
        "0123456789ab@:/?", "agent-run-7", &profile, &error) == MAELYS_EGRESS_OK);
    CHECK(profile != NULL);
    CHECK(maelys_egress_profile_environment_count(profile) == 8u);
    const char *name = NULL;
    const char *value = NULL;
    CHECK(maelys_egress_profile_environment_at(profile, 0u, &name, &value) ==
          MAELYS_EGRESS_OK);
    CHECK(strcmp(name, "HTTP_PROXY") == 0);
    CHECK(strcmp(value,
        "http://worker%2Bname:0123456789ab%40%3A%2F%3F@127.0.0.1:8080") == 0);
    CHECK(maelys_egress_profile_environment_at(profile, 4u, &name, &value) ==
          MAELYS_EGRESS_OK);
    CHECK(strncmp(value, "socks5h://", 10u) == 0);
    CHECK(maelys_egress_profile_environment_at(profile, 7u, &name, &value) ==
          MAELYS_EGRESS_OK && value[0] == '\0');
    CHECK(strcmp(maelys_egress_profile_invocation_id(profile), "agent-run-7") == 0);
    maelys_egress_config_t *config = NULL;
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_profile_apply(profile, config, &error) == MAELYS_EGRESS_OK);
    char invocation[EGRESS_MAX_INVOCATION_ID + 1u];
    CHECK(egress_credentials_lookup(config, "worker+name", "0123456789ab@:/?",
                                  invocation, NULL));
    CHECK(strcmp(invocation, "agent-run-7") == 0);
    maelys_egress_config_destroy(config);
    maelys_egress_profile_destroy(profile);

#if defined(__APPLE__)
    char directory[] = "/private/tmp/maelys-egress-profile-XXXXXX";
#else
    char directory[] = "/tmp/maelys-egress-profile-XXXXXX";
#endif
    CHECK(mkdtemp(directory) != NULL);
    CHECK(chmod(directory, 0700) == 0);
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    CHECK(snprintf(socket_path, sizeof(socket_path), "%s/egress.sock", directory) > 0);
    profile = NULL;
    CHECK(maelys_egress_profile_create_endpoint_bound(
        "127.0.0.1", 43119u, "worker", "agent-run-8",
        &profile, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_profile_environment_at(profile, 0u, &name, &value) ==
          MAELYS_EGRESS_OK);
    CHECK(strcmp(value, "http://127.0.0.1:43119") == 0);
    CHECK(strchr(value, '@') == NULL);
    CHECK(maelys_egress_profile_environment_at(profile, 4u, &name, &value) ==
          MAELYS_EGRESS_OK);
    CHECK(strcmp(value, "socks5h://127.0.0.1:43119") == 0);
    config = NULL;
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_listen_unix(
        config, socket_path, strlen(socket_path),
        MAELYS_EGRESS_UNIX_PEER_SAME_EUID, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_profile_apply(profile, config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_principal_quota(
        config, "worker", 4u, 8192u, &error) == MAELYS_EGRESS_OK);

    static const char bound_http[] =
        "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n";
    egress_proxy_request_t bound_request;
    memset(&bound_request, 0, sizeof(bound_request));
    CHECK(egress_parse_http_request((const unsigned char *)bound_http,
        sizeof(bound_http) - 1u, config, &bound_request, &error) == 1);
    CHECK(bound_request.principal_index == 0u);
    CHECK(strcmp(bound_request.invocation_id, "agent-run-8") == 0);
    egress_proxy_request_clear(&bound_request);
    static const char forbidden_credential[] =
        "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n"
        "Proxy-Authorization: Bearer 0123456789abcdef\r\n\r\n";
    CHECK(egress_parse_http_request((const unsigned char *)forbidden_credential,
        sizeof(forbidden_credential) - 1u, config, &bound_request, &error) == -2);
    maelys_egress_error_free(error); error = NULL;

    int phase = 0;
    size_t consumed = 0u;
    unsigned char socks_response[10] = {0};
    size_t response_length = 0u;
    char bound_invocation[EGRESS_MAX_INVOCATION_ID + 1u] = {0};
    size_t bound_principal = SIZE_MAX;
    const unsigned char greeting[] = {5u, 1u, 0u};
    CHECK(egress_parse_socks_frame(greeting, sizeof(greeting), config, &phase,
        &consumed, socks_response, &response_length, &bound_request,
        bound_invocation, &bound_principal) == 1);
    CHECK(phase == 2 && response_length == 2u && socks_response[1] == 0u);
    CHECK(bound_principal == 0u && strcmp(bound_invocation, "agent-run-8") == 0);
    CHECK(maelys_egress_config_add_principal(config, "other",
        "0123456789abcdef", NULL, &error) == MAELYS_EGRESS_ERR_STATE);
    maelys_egress_error_free(error); error = NULL;
    maelys_egress_config_destroy(config);
    maelys_egress_profile_destroy(profile);
    CHECK(rmdir(directory) == 0);
    maelys_egress_error_free(error);
}

static int listener_create(uint16_t *out_port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = 0
    };
    (void)inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 8) != 0) {
        (void)close(fd); return -1;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, (struct sockaddr *)&address, &length) != 0) {
        (void)close(fd); return -1;
    }
    *out_port = ntohs(address.sin_port);
    return fd;
}

static int send_all(int fd, const void *bytes, size_t length) {
    const unsigned char *cursor = bytes;
    while (length) {
        ssize_t sent = send(fd, cursor, length, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return 0;
        cursor += (size_t)sent;
        length -= (size_t)sent;
    }
    return 1;
}

static int receive_exact(int fd, void *bytes, size_t length) {
    unsigned char *cursor = bytes;
    while (length) {
        ssize_t received = recv(fd, cursor, length, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return 0;
        cursor += (size_t)received;
        length -= (size_t)received;
    }
    return 1;
}

static int connect_loopback(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port)
    };
    (void)inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(fd); return -1;
    }
    return fd;
}

static int connect_unix_path(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t length = strlen(path);
    if (length >= sizeof(address.sun_path)) {
        (void)close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(address.sun_path, path, length + 1u);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

typedef struct upstream_context {
    int listener;
    int failed;
} upstream_context_t;

static void *upstream_main(void *opaque) {
    upstream_context_t *context = opaque;
    for (int request = 0; request < 3; ++request) {
        int client;
        do { client = accept(context->listener, NULL, NULL); }
        while (client < 0 && errno == EINTR);
        if (client < 0) { context->failed = 1; break; }
        if (request < 2) {
            char ping[4];
            if (!receive_exact(client, ping, sizeof(ping)) ||
                memcmp(ping, "ping", 4u) != 0 ||
                !send_all(client, ping, sizeof(ping))) context->failed = 1;
        } else {
            char request_bytes[2048];
            size_t used = 0u;
            while (used + 1u < sizeof(request_bytes)) {
                ssize_t count = recv(client, request_bytes + used,
                                     sizeof(request_bytes) - used - 1u, 0);
                if (count <= 0) break;
                used += (size_t)count;
                request_bytes[used] = '\0';
                if (strstr(request_bytes, "\r\n\r\n")) break;
            }
            if (strncmp(request_bytes, "GET /hello HTTP/1.1\r\n", 21u) != 0 ||
                strstr(request_bytes, "Proxy-Authorization") ||
                !strstr(request_bytes, "Connection: close\r\n")) {
                context->failed = 1;
            }
            static const char response[] =
                "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
            if (!send_all(client, response, sizeof(response) - 1u)) context->failed = 1;
        }
        (void)shutdown(client, SHUT_RDWR);
        (void)close(client);
    }
    (void)close(context->listener);
    return NULL;
}

typedef struct proxy_context {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    maelys_egress_policy_t *policy;
    maelys_egress_config_t *config;
    maelys_egress_server_t *server;
    uint16_t port;
    int ready;
    maelys_egress_result_t result;
    char *error;
} proxy_context_t;

static void *proxy_main(void *opaque) {
    proxy_context_t *context = opaque;
    maelys_egress_server_t *server = NULL;
    maelys_egress_result_t result = maelys_egress_server_create(
        context->policy, context->config, &server, &context->error);
    (void)pthread_mutex_lock(&context->lock);
    context->server = server;
    context->result = result;
    context->port = server ? maelys_egress_server_port(server) : 0u;
    context->ready = 1;
    (void)pthread_cond_signal(&context->condition);
    (void)pthread_mutex_unlock(&context->lock);
    if (result == MAELYS_EGRESS_OK) {
        context->result = maelys_egress_server_run(server, &context->error);
    }
    if (context->result == MAELYS_EGRESS_OK) {
        char *second_error = NULL;
        CHECK(maelys_egress_server_run(server, &second_error) ==
              MAELYS_EGRESS_ERR_STATE);
        CHECK(second_error != NULL);
        maelys_egress_error_free(second_error);
    }
    maelys_egress_server_destroy(server);
    return NULL;
}

static void receipt_count(void *opaque, const maelys_egress_receipt_t *receipt) {
    int *count = opaque;
    CHECK(receipt != NULL);
    CHECK(strlen(maelys_egress_receipt_policy_digest_hex(receipt)) == 64u);
    ++*count;
}

static int read_http_header(int fd, char *buffer, size_t capacity) {
    size_t used = 0u;
    while (used + 1u < capacity) {
        ssize_t count = recv(fd, buffer + used, 1u, 0);
        if (count <= 0) return 0;
        used += 1u;
        buffer[used] = '\0';
        if (strstr(buffer, "\r\n\r\n")) return 1;
    }
    return 0;
}

static void test_proxy_end_to_end(void) {
    uint16_t upstream_port = 0u;
    int upstream_listener = listener_create(&upstream_port);
    CHECK(upstream_listener >= 0);
    upstream_context_t upstream = {.listener = upstream_listener};
    pthread_t upstream_thread;
    CHECK(pthread_create(&upstream_thread, NULL, upstream_main, &upstream) == 0);

    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    char *error = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "127.0.0.1", upstream_port, 1,
                                       &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_authentication(
        config, "maelys", "0123456789abcdef", &error) == MAELYS_EGRESS_OK);
    maelys_egress_tls_provider_t *listener_tls = make_passthrough_provider();
    CHECK(maelys_egress_config_set_tls_listener(config, listener_tls, &error) ==
          MAELYS_EGRESS_OK);
    maelys_egress_tls_provider_release(listener_tls);
    CHECK(maelys_egress_config_set_limits(config, 16u, 32768u, 3000u, 3000u,
                                       &error) == MAELYS_EGRESS_OK);
    int receipts = 0;
    maelys_egress_config_set_receipt_sink(config, receipt_count, &receipts);

    proxy_context_t proxy = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .policy = policy,
        .config = config
    };
    pthread_t proxy_thread;
    CHECK(pthread_create(&proxy_thread, NULL, proxy_main, &proxy) == 0);
    (void)pthread_mutex_lock(&proxy.lock);
    while (!proxy.ready) (void)pthread_cond_wait(&proxy.condition, &proxy.lock);
    (void)pthread_mutex_unlock(&proxy.lock);
    CHECK(proxy.result == MAELYS_EGRESS_OK);
    CHECK(proxy.port != 0u);

    int client = connect_loopback(proxy.port);
    CHECK(client >= 0);
    char connect_request[512];
    int connect_length = snprintf(connect_request, sizeof(connect_request),
        "CONNECT 127.0.0.1:%u HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
        "Proxy-Authorization: Bearer 0123456789abcdef\r\n\r\n",
        (unsigned int)upstream_port, (unsigned int)upstream_port);
    CHECK(connect_length > 0 && send_all(client, connect_request, (size_t)connect_length));
    char response[1024] = {0};
    CHECK(read_http_header(client, response, sizeof(response)));
    CHECK(strncmp(response, "HTTP/1.1 200", 12u) == 0);
    CHECK(send_all(client, "ping", 4u));
    char pong[4];
    CHECK(receive_exact(client, pong, sizeof(pong)) && memcmp(pong, "ping", 4u) == 0);
    (void)close(client);

    client = connect_loopback(proxy.port);
    CHECK(client >= 0);
    const unsigned char greeting[] = {5u, 1u, 2u};
    unsigned char small[10];
    CHECK(send_all(client, greeting, sizeof(greeting)) && receive_exact(client, small, 2u));
    CHECK(small[0] == 5u && small[1] == 2u);
    unsigned char auth[1u + 1u + 6u + 1u + 16u] = {1u, 6u};
    memcpy(auth + 2u, "maelys", 6u);
    auth[8] = 16u;
    memcpy(auth + 9u, "0123456789abcdef", 16u);
    CHECK(send_all(client, auth, sizeof(auth)) && receive_exact(client, small, 2u));
    CHECK(small[1] == 0u);
    unsigned char socks_request[10] = {5u, 1u, 0u, 1u, 127u, 0u, 0u, 1u,
        (unsigned char)(upstream_port >> 8u), (unsigned char)upstream_port};
    CHECK(send_all(client, socks_request, sizeof(socks_request)) &&
          receive_exact(client, small, sizeof(small)));
    CHECK(small[1] == 0u);
    CHECK(send_all(client, "ping", 4u));
    CHECK(receive_exact(client, pong, sizeof(pong)) && memcmp(pong, "ping", 4u) == 0);
    (void)close(client);

    client = connect_loopback(proxy.port);
    CHECK(client >= 0);
    char forward_request[768];
    int forward_length = snprintf(forward_request, sizeof(forward_request),
        "GET http://127.0.0.1:%u/hello HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
        "Proxy-Authorization: Bearer 0123456789abcdef\r\nProxy-Connection: keep-alive\r\n\r\n",
        (unsigned int)upstream_port, (unsigned int)upstream_port);
    CHECK(forward_length > 0 && send_all(client, forward_request, (size_t)forward_length));
    size_t total = 0u;
    while (total + 1u < sizeof(response)) {
        ssize_t count = recv(client, response + total, sizeof(response) - total - 1u, 0);
        if (count <= 0) break;
        total += (size_t)count;
    }
    response[total] = '\0';
    CHECK(strstr(response, "HTTP/1.1 200 OK") != NULL);
    CHECK(strstr(response, "\r\n\r\nhello") != NULL);
    (void)close(client);

    client = connect_loopback(proxy.port);
    CHECK(client >= 0);
    static const char denied[] =
        "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n"
        "Proxy-Authorization: Bearer 0123456789abcdef\r\n\r\n";
    CHECK(send_all(client, denied, sizeof(denied) - 1u));
    memset(response, 0, sizeof(response));
    CHECK(read_http_header(client, response, sizeof(response)));
    CHECK(strncmp(response, "HTTP/1.1 403", 12u) == 0);
    (void)close(client);

    client = connect_loopback(proxy.port);
    CHECK(client >= 0);
    static const char malformed[] =
        "CONNECT example.com:443 HTTP/1.1\r\nHost: other.example:443\r\n"
        "Proxy-Authorization: Bearer 0123456789abcdef\r\n\r\n";
    CHECK(send_all(client, malformed, sizeof(malformed) - 1u));
    memset(response, 0, sizeof(response));
    CHECK(read_http_header(client, response, sizeof(response)));
    CHECK(strncmp(response, "HTTP/1.1 400", 12u) == 0);
    (void)close(client);

    struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
    (void)nanosleep(&pause, NULL);
    CHECK(maelys_egress_server_stop(proxy.server) == MAELYS_EGRESS_OK);
    CHECK(pthread_join(proxy_thread, NULL) == 0);
    CHECK(pthread_join(upstream_thread, NULL) == 0);
    CHECK(proxy.result == MAELYS_EGRESS_OK);
    CHECK(proxy.error == NULL);
    CHECK(upstream.failed == 0);
    CHECK(receipts >= 5);
    maelys_egress_error_free(proxy.error);
    maelys_egress_error_free(error);
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    (void)pthread_cond_destroy(&proxy.condition);
    (void)pthread_mutex_destroy(&proxy.lock);
}

typedef struct unix_upstream_context {
    int listener;
    int failed;
} unix_upstream_context_t;

static void *unix_upstream_main(void *opaque) {
    unix_upstream_context_t *context = opaque;
    for (int index = 0; index < 2; ++index) {
        int client;
        do { client = accept(context->listener, NULL, NULL); }
        while (client < 0 && errno == EINTR);
        char ping[4];
        if (client < 0 || !receive_exact(client, ping, sizeof(ping)) ||
            memcmp(ping, "ping", sizeof(ping)) != 0 ||
            !send_all(client, ping, sizeof(ping))) context->failed = 1;
        if (client >= 0) (void)close(client);
    }
    (void)close(context->listener);
    return NULL;
}

static void test_unix_listener(void) {
#if defined(__APPLE__)
    char directory[] = "/private/tmp/maelys-egress-unix-XXXXXX";
#else
    char directory[] = "/tmp/maelys-egress-unix-XXXXXX";
#endif
    CHECK(mkdtemp(directory) != NULL);
    CHECK(chmod(directory, 0700) == 0);
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    CHECK(snprintf(socket_path, sizeof(socket_path), "%s/proxy.sock", directory) > 0);

    maelys_egress_config_t *validation = NULL;
    char *error = NULL;
    CHECK(maelys_egress_config_create(&validation, &error) == MAELYS_EGRESS_OK);
    int existing = open(socket_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(existing >= 0);
    if (existing >= 0) (void)close(existing);
    maelys_egress_result_t unix_result = maelys_egress_config_set_listen_unix(
        validation, socket_path, strlen(socket_path),
        MAELYS_EGRESS_UNIX_PEER_AUTHENTICATED, &error);
    if (unix_result != MAELYS_EGRESS_ERR_STATE) {
        fprintf(stderr, "Unix pre-existing result=%d error=%s\n", (int)unix_result,
                error ? error : "none");
    }
    CHECK(unix_result == MAELYS_EGRESS_ERR_STATE);
    maelys_egress_error_free(error); error = NULL;
    CHECK(unlink(socket_path) == 0);
    char embedded[] = {'/', 'x', '\0', 'y', '\0'};
    CHECK(maelys_egress_config_set_listen_unix(
        validation, embedded, 4u, MAELYS_EGRESS_UNIX_PEER_AUTHENTICATED,
        &error) == MAELYS_EGRESS_ERR_ARGUMENT);
    maelys_egress_error_free(error); error = NULL;
    char too_long[256];
    memset(too_long, 'x', sizeof(too_long));
    too_long[0] = '/';
    too_long[sizeof(too_long) - 1u] = '\0';
    CHECK(maelys_egress_config_set_listen_unix(
        validation, too_long, strlen(too_long),
        MAELYS_EGRESS_UNIX_PEER_AUTHENTICATED,
        &error) == MAELYS_EGRESS_ERR_ARGUMENT);
    maelys_egress_error_free(error); error = NULL;

    char real_parent[sizeof(socket_path)];
    char linked_parent[sizeof(socket_path)];
    char linked_socket[sizeof(socket_path)];
    CHECK(snprintf(real_parent, sizeof(real_parent), "%s/real", directory) > 0);
    CHECK(snprintf(linked_parent, sizeof(linked_parent), "%s/link", directory) > 0);
    CHECK(snprintf(linked_socket, sizeof(linked_socket), "%s/proxy.sock", linked_parent) > 0);
    CHECK(mkdir(real_parent, 0700) == 0);
    CHECK(symlink(real_parent, linked_parent) == 0);
    CHECK(maelys_egress_config_set_listen_unix(
        validation, linked_socket, strlen(linked_socket),
        MAELYS_EGRESS_UNIX_PEER_AUTHENTICATED, &error) == MAELYS_EGRESS_ERR_DENIED);
    maelys_egress_error_free(error); error = NULL;
    CHECK(unlink(linked_parent) == 0);
    CHECK(rmdir(real_parent) == 0);

    CHECK(chmod(directory, 0755) == 0);
    CHECK(maelys_egress_config_set_listen_unix(
        validation, socket_path, strlen(socket_path),
        MAELYS_EGRESS_UNIX_PEER_AUTHENTICATED, &error) == MAELYS_EGRESS_ERR_DENIED);
    maelys_egress_error_free(error); error = NULL;
    CHECK(chmod(directory, 0700) == 0);
    maelys_egress_config_destroy(validation);

    uint16_t upstream_port = 0u;
    int listener = listener_create(&upstream_port);
    CHECK(listener >= 0);
    unix_upstream_context_t upstream = {.listener = listener};
    pthread_t upstream_thread;
    CHECK(pthread_create(&upstream_thread, NULL, unix_upstream_main, &upstream) == 0);
    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(
        policy, "127.0.0.1", upstream_port, 1, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    unix_result = maelys_egress_config_set_listen_unix(
        config, socket_path, strlen(socket_path),
        MAELYS_EGRESS_UNIX_PEER_SAME_EUID, &error);
    if (unix_result != MAELYS_EGRESS_OK) {
        fprintf(stderr, "Unix valid result=%d error=%s\n", (int)unix_result,
                error ? error : "none");
    }
    CHECK(unix_result == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_allow_unauthenticated_loopback(config, 1, &error) ==
          MAELYS_EGRESS_OK);
    maelys_egress_server_t *must_authenticate = NULL;
    CHECK(maelys_egress_server_create(policy, config, &must_authenticate, &error) ==
          MAELYS_EGRESS_ERR_DENIED);
    CHECK(must_authenticate == NULL);
    maelys_egress_error_free(error); error = NULL;
    CHECK(maelys_egress_config_set_authentication(
        config, "maelys", "0123456789abcdef", &error) == MAELYS_EGRESS_OK);
    int receipts = 0;
    maelys_egress_config_set_receipt_sink(config, receipt_count, &receipts);

    CHECK(chmod(directory, 0755) == 0);
    maelys_egress_server_t *changed_parent = NULL;
    CHECK(maelys_egress_server_create(policy, config, &changed_parent, &error) ==
          MAELYS_EGRESS_ERR_DENIED);
    CHECK(changed_parent == NULL);
    maelys_egress_error_free(error); error = NULL;
    CHECK(chmod(directory, 0700) == 0);

    proxy_context_t proxy = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .policy = policy,
        .config = config
    };
    pthread_t proxy_thread;
    CHECK(pthread_create(&proxy_thread, NULL, proxy_main, &proxy) == 0);
    (void)pthread_mutex_lock(&proxy.lock);
    while (!proxy.ready) (void)pthread_cond_wait(&proxy.condition, &proxy.lock);
    (void)pthread_mutex_unlock(&proxy.lock);
    CHECK(proxy.result == MAELYS_EGRESS_OK);
    CHECK(proxy.port == 0u);
    struct stat socket_status;
    CHECK(lstat(socket_path, &socket_status) == 0 && S_ISSOCK(socket_status.st_mode));
    CHECK((socket_status.st_mode & 0777) == 0600);

    int client = connect_unix_path(socket_path);
    CHECK(client >= 0);
    char wrong_request[512];
    int wrong_length = snprintf(wrong_request, sizeof(wrong_request),
        "CONNECT 127.0.0.1:%u HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
        "Proxy-Authorization: Bearer fedcba9876543210\r\n\r\n",
        (unsigned int)upstream_port, (unsigned int)upstream_port);
    CHECK(wrong_length > 0 && send_all(client, wrong_request, (size_t)wrong_length));
    char denied[512] = {0};
    CHECK(read_http_header(client, denied, sizeof(denied)));
    CHECK(strncmp(denied, "HTTP/1.1 407", 12u) == 0);
    (void)close(client);

    client = connect_unix_path(socket_path);
    CHECK(client >= 0);
    char request[512];
    int request_length = snprintf(request, sizeof(request),
        "CONNECT 127.0.0.1:%u HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
        "Proxy-Authorization: Bearer 0123456789abcdef\r\n\r\n",
        (unsigned int)upstream_port, (unsigned int)upstream_port);
    CHECK(request_length > 0 && send_all(client, request, (size_t)request_length));
    char response[512] = {0};
    CHECK(read_http_header(client, response, sizeof(response)));
    CHECK(strncmp(response, "HTTP/1.1 200", 12u) == 0);
    CHECK(send_all(client, "ping", 4u));
    char pong[4];
    CHECK(receive_exact(client, pong, sizeof(pong)) && memcmp(pong, "ping", 4u) == 0);
    (void)close(client);

    client = connect_unix_path(socket_path);
    CHECK(client >= 0);
    const unsigned char greeting[] = {5u, 1u, 2u};
    unsigned char small[10];
    CHECK(send_all(client, greeting, sizeof(greeting)) && receive_exact(client, small, 2u));
    unsigned char auth[25] = {1u, 6u};
    memcpy(auth + 2u, "maelys", 6u);
    auth[8] = 16u;
    memcpy(auth + 9u, "0123456789abcdef", 16u);
    CHECK(send_all(client, auth, sizeof(auth)) && receive_exact(client, small, 2u));
    unsigned char socks_request[10] = {5u, 1u, 0u, 1u, 127u, 0u, 0u, 1u,
        (unsigned char)(upstream_port >> 8u), (unsigned char)upstream_port};
    CHECK(send_all(client, socks_request, sizeof(socks_request)) &&
          receive_exact(client, small, sizeof(small)));
    CHECK(small[1] == 0u);
    CHECK(send_all(client, "ping", 4u));
    CHECK(receive_exact(client, pong, sizeof(pong)) && memcmp(pong, "ping", 4u) == 0);
    (void)close(client);

    CHECK(pthread_join(upstream_thread, NULL) == 0);
    CHECK(upstream.failed == 0);
    CHECK(maelys_egress_server_stop(proxy.server) == MAELYS_EGRESS_OK);
    CHECK(pthread_join(proxy_thread, NULL) == 0);
    CHECK(access(socket_path, F_OK) != 0);
    CHECK(receipts >= 3);

    maelys_egress_server_t *replacement = NULL;
    CHECK(maelys_egress_server_create(policy, config, &replacement, &error) == MAELYS_EGRESS_OK);
    CHECK(unlink(socket_path) == 0);
    int replacement_file = open(socket_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(replacement_file >= 0);
    if (replacement_file >= 0) (void)close(replacement_file);
    maelys_egress_server_destroy(replacement);
    CHECK(lstat(socket_path, &socket_status) == 0 && S_ISREG(socket_status.st_mode));
    CHECK(unlink(socket_path) == 0);

    maelys_egress_error_free(proxy.error);
    maelys_egress_error_free(error);
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    (void)pthread_cond_destroy(&proxy.condition);
    (void)pthread_mutex_destroy(&proxy.lock);
    CHECK(rmdir(directory) == 0);
}

#define RELAY_STRESS_BYTES (256u * 1024u)

typedef struct relay_stress_context {
    int listener;
    int failed;
} relay_stress_context_t;

static unsigned char stress_byte(size_t index) {
    return (unsigned char)((index * 131u + 17u) & 0xffu);
}

static void *relay_stress_main(void *opaque) {
    relay_stress_context_t *context = opaque;
    int client;
    do { client = accept(context->listener, NULL, NULL); }
    while (client < 0 && errno == EINTR);
    size_t received_total = 0u;
    unsigned char buffer[1024];
    while (client >= 0) {
        ssize_t received = recv(client, buffer, sizeof(buffer), 0);
        if (received < 0 && errno == EINTR) continue;
        if (received < 0) { context->failed = 1; break; }
        if (received == 0) break;
        for (ssize_t i = 0; i < received; ++i) {
            if (buffer[i] != stress_byte(received_total + (size_t)i)) {
                context->failed = 1;
            }
        }
        received_total += (size_t)received;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    if (received_total != RELAY_STRESS_BYTES) context->failed = 1;
    for (size_t offset = 0u; client >= 0 && offset < RELAY_STRESS_BYTES;) {
        size_t chunk = RELAY_STRESS_BYTES - offset;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        for (size_t i = 0; i < chunk; ++i) buffer[i] = stress_byte(offset + i);
        if (!send_all(client, buffer, chunk)) { context->failed = 1; break; }
        offset += chunk;
    }
    if (client >= 0) {
        (void)shutdown(client, SHUT_WR);
        (void)close(client);
    }
    (void)close(context->listener);
    return NULL;
}

static void test_relay_backpressure_and_half_close(void) {
    uint16_t upstream_port = 0u;
    int upstream_listener = listener_create(&upstream_port);
    CHECK(upstream_listener >= 0);
    relay_stress_context_t upstream = {.listener = upstream_listener};
    pthread_t upstream_thread;
    CHECK(pthread_create(&upstream_thread, NULL, relay_stress_main, &upstream) == 0);

    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    char *error = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "127.0.0.1", upstream_port, 1,
                                       &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_allow_unauthenticated_loopback(config, 1, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_limits(config, 4u, EGRESS_MIN_BUFFER_BYTES,
                                        3000u, 10000u, &error) == MAELYS_EGRESS_OK);
    proxy_context_t proxy = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .policy = policy,
        .config = config
    };
    pthread_t proxy_thread;
    CHECK(pthread_create(&proxy_thread, NULL, proxy_main, &proxy) == 0);
    (void)pthread_mutex_lock(&proxy.lock);
    while (!proxy.ready) (void)pthread_cond_wait(&proxy.condition, &proxy.lock);
    (void)pthread_mutex_unlock(&proxy.lock);
    CHECK(proxy.result == MAELYS_EGRESS_OK);

    int client = connect_loopback(proxy.port);
    CHECK(client >= 0);
    char request[512];
    int request_length = snprintf(request, sizeof(request),
        "CONNECT 127.0.0.1:%u HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
        (unsigned int)upstream_port, (unsigned int)upstream_port);
    CHECK(request_length > 0 && send_all(client, request, (size_t)request_length));
    char response[512] = {0};
    CHECK(read_http_header(client, response, sizeof(response)));
    CHECK(strncmp(response, "HTTP/1.1 200", 12u) == 0);
    unsigned char block[4096];
    for (size_t offset = 0u; offset < RELAY_STRESS_BYTES; offset += sizeof(block)) {
        for (size_t i = 0; i < sizeof(block); ++i) block[i] = stress_byte(offset + i);
        CHECK(send_all(client, block, sizeof(block)));
    }
    CHECK(shutdown(client, SHUT_WR) == 0);
    size_t received_total = 0u;
    while (received_total < RELAY_STRESS_BYTES) {
        ssize_t received = recv(client, block, sizeof(block), 0);
        if (received < 0 && errno == EINTR) continue;
        CHECK(received > 0);
        if (received <= 0) break;
        for (ssize_t i = 0; i < received; ++i) {
            CHECK(block[i] == stress_byte(received_total + (size_t)i));
        }
        received_total += (size_t)received;
    }
    CHECK(received_total == RELAY_STRESS_BYTES);
    CHECK(recv(client, block, sizeof(block), 0) == 0);
    (void)close(client);

    CHECK(pthread_join(upstream_thread, NULL) == 0);
    CHECK(upstream.failed == 0);
    CHECK(maelys_egress_server_stop(proxy.server) == MAELYS_EGRESS_OK);
    CHECK(pthread_join(proxy_thread, NULL) == 0);
    CHECK(proxy.result == MAELYS_EGRESS_OK && proxy.error == NULL);
    maelys_egress_error_free(error);
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    (void)pthread_cond_destroy(&proxy.condition);
    (void)pthread_mutex_destroy(&proxy.lock);
}

typedef struct sni_upstream_context {
    int listener;
    size_t expected_length;
    unsigned char expected[EGRESS_HANDSHAKE_MAX];
    int failed;
} sni_upstream_context_t;

static void *sni_upstream_main(void *opaque) {
    sni_upstream_context_t *context = opaque;
    int client;
    do { client = accept(context->listener, NULL, NULL); }
    while (client < 0 && errno == EINTR);
    unsigned char received[EGRESS_HANDSHAKE_MAX];
    if (client < 0 || !receive_exact(client, received, context->expected_length) ||
        memcmp(received, context->expected, context->expected_length) != 0 ||
        !send_all(client, "ok", 2u)) context->failed = 1;
    if (client >= 0) { (void)shutdown(client, SHUT_RDWR); (void)close(client); }
    (void)close(context->listener);
    return NULL;
}

typedef struct sni_receipt_context {
    int count;
    int verified;
} sni_receipt_context_t;

static void capture_sni_receipt(
    void *opaque, const maelys_egress_receipt_t *receipt) {
    sni_receipt_context_t *context = opaque;
    ++context->count;
    context->verified = maelys_egress_receipt_tls_sni_verified(receipt);
}

static void test_sni_guard_end_to_end(void) {
    uint16_t upstream_port = 0u;
    int listener = listener_create(&upstream_port);
    CHECK(listener >= 0);
    sni_upstream_context_t upstream = {.listener = listener};
    upstream.expected_length = make_client_hello("localhost", upstream.expected);
    pthread_t upstream_thread;
    CHECK(pthread_create(&upstream_thread, NULL, sni_upstream_main, &upstream) == 0);

    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    char *error = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "localhost", upstream_port, 1, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_require_tls_sni(policy, "localhost", upstream_port, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_allow_unauthenticated_loopback(config, 1, &error) ==
          MAELYS_EGRESS_OK);
    sni_receipt_context_t receipt = {0};
    maelys_egress_config_set_receipt_sink(config, capture_sni_receipt, &receipt);
    proxy_context_t proxy = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .policy = policy,
        .config = config
    };
    pthread_t proxy_thread;
    CHECK(pthread_create(&proxy_thread, NULL, proxy_main, &proxy) == 0);
    (void)pthread_mutex_lock(&proxy.lock);
    while (!proxy.ready) (void)pthread_cond_wait(&proxy.condition, &proxy.lock);
    (void)pthread_mutex_unlock(&proxy.lock);
    CHECK(proxy.result == MAELYS_EGRESS_OK);

    int client = connect_loopback(proxy.port);
    CHECK(client >= 0);
    char request[512];
    int request_length = snprintf(request, sizeof(request),
        "CONNECT localhost:%u HTTP/1.1\r\nHost: localhost:%u\r\n\r\n",
        (unsigned int)upstream_port, (unsigned int)upstream_port);
    CHECK(request_length > 0 && send_all(client, request, (size_t)request_length));
    char response[512] = {0};
    CHECK(read_http_header(client, response, sizeof(response)));
    CHECK(strncmp(response, "HTTP/1.1 200", 12u) == 0);
    size_t split = upstream.expected_length / 2u;
    CHECK(send_all(client, upstream.expected, split));
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
    (void)nanosleep(&delay, NULL);
    CHECK(send_all(client, upstream.expected + split, upstream.expected_length - split));
    char ok[2];
    CHECK(receive_exact(client, ok, sizeof(ok)) && memcmp(ok, "ok", 2u) == 0);
    (void)close(client);
    CHECK(pthread_join(upstream_thread, NULL) == 0);
    CHECK(upstream.failed == 0);
    (void)nanosleep(&delay, NULL);
    CHECK(maelys_egress_server_stop(proxy.server) == MAELYS_EGRESS_OK);
    CHECK(pthread_join(proxy_thread, NULL) == 0);
    CHECK(receipt.count == 1 && receipt.verified == 1);
    CHECK(proxy.result == MAELYS_EGRESS_OK && proxy.error == NULL);
    maelys_egress_error_free(error);
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    (void)pthread_cond_destroy(&proxy.condition);
    (void)pthread_mutex_destroy(&proxy.lock);
}

int main(void) {
    test_version_and_tls_seam();
    test_policy_fail_closed();
    test_address_classification();
    test_http_parser_adversarial();
    test_execution_profile();
    test_tls_client_hello_identity();
    test_proxy_end_to_end();
    test_unix_listener();
    test_relay_backpressure_and_half_close();
    test_sni_guard_end_to_end();
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    puts("all egress checks passed");
    return 0;
}
