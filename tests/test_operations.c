#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "maelys/egress.h"
#include "maelys/egress_channel.h"
#include "maelys/egress_client.h"
#include "src/internal.h"
#include "common/bootstrap.h"
#include "maelys/sys/fdpass.h"
#include "tests/tls_socket_fixture.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); ++failures; \
} } while (0)

static int send_all(int fd, const void *data, size_t length) {
    const unsigned char *bytes = data;
    while (length) {
        ssize_t amount = send(fd, bytes, length, 0);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return 0;
        bytes += (size_t)amount;
        length -= (size_t)amount;
    }
    return 1;
}

static int receive_header(int fd, char *buffer, size_t capacity) {
    size_t used = 0u;
    while (used + 1u < capacity) {
        ssize_t amount = recv(fd, buffer + used, capacity - used - 1u, 0);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) break;
        used += (size_t)amount;
        buffer[used] = '\0';
        if (strstr(buffer, "\r\n\r\n")) return 1;
    }
    return 0;
}

static int listener_create(uint16_t *out_port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = 0};
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        (void)close(fd); return -1;
    }
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 8) != 0) { (void)close(fd); return -1; }
    socklen_t length = sizeof(address);
    if (getsockname(fd, (struct sockaddr *)&address, &length) != 0) {
        (void)close(fd); return -1;
    }
    *out_port = ntohs(address.sin_port);
    return fd;
}

static int connect_loopback(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        (void)close(fd); return -1;
    }
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return -1;
    }
    return fd;
}

static maelys_egress_result_t test_attest(
    void *context, const void *canonical, size_t canonical_length,
    unsigned char *signature, size_t signature_capacity,
    size_t *out_signature_length, char **out_error) {
    (void)context;
    if (out_error) *out_error = NULL;
    if (!canonical || canonical_length == 0u || !signature ||
        signature_capacity < 4u || !out_signature_length) {
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    const unsigned char *bytes = canonical;
    unsigned char folded = 0u;
    for (size_t i = 0; i < canonical_length; ++i) folded ^= bytes[i];
    signature[0] = 0x4du;
    signature[1] = 0x41u;
    signature[2] = 0x45u;
    signature[3] = folded;
    *out_signature_length = 4u;
    return MAELYS_EGRESS_OK;
}

typedef struct upstream_context { int listener; int failed; } upstream_context_t;
static void *upstream_main(void *opaque) {
    upstream_context_t *context = opaque;
    int client;
    do { client = accept(context->listener, NULL, NULL); }
    while (client < 0 && errno == EINTR);
    char bytes[16];
    while (client >= 0) {
        ssize_t amount = recv(client, bytes, sizeof(bytes), 0);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) break;
        if (!send_all(client, bytes, (size_t)amount)) { context->failed = 1; break; }
    }
    if (client >= 0) (void)close(client);
    (void)close(context->listener);
    return NULL;
}

typedef struct connector_receipts {
    size_t count;
    int saw_connector;
    int saw_denied;
    uint64_t guarded_client_bytes;
} connector_receipts_t;

static void connector_receipt_sink(
    void *opaque, const maelys_egress_receipt_t *receipt) {
    connector_receipts_t *capture = opaque;
    ++capture->count;
    if (maelys_egress_receipt_protocol(receipt) ==
        MAELYS_EGRESS_PROTOCOL_CONNECTOR) {
        capture->saw_connector = 1;
        if (maelys_egress_receipt_result(receipt) == MAELYS_EGRESS_ERR_DENIED) {
            capture->saw_denied = 1;
        }
        if (maelys_egress_receipt_bytes_from_client(receipt) >
            capture->guarded_client_bytes) {
            capture->guarded_client_bytes =
                maelys_egress_receipt_bytes_from_client(receipt);
        }
    }
}

typedef struct guard_upstream_context {
    int listener;
    int received_application_bytes;
} guard_upstream_context_t;

static void *guard_upstream_main(void *opaque) {
    guard_upstream_context_t *context = opaque;
    int client;
    do { client = accept(context->listener, NULL, NULL); }
    while (client < 0 && errno == EINTR);
    unsigned char byte;
    ssize_t amount;
    do { amount = client >= 0 ? recv(client, &byte, 1u, 0) : -1; }
    while (amount < 0 && errno == EINTR);
    context->received_application_bytes = amount > 0;
    if (client >= 0) (void)close(client);
    (void)close(context->listener);
    return NULL;
}

#define CONNECTOR_CONCURRENCY 8u

typedef struct multi_upstream_context {
    int listener;
    size_t expected;
    int failed;
} multi_upstream_context_t;

static void *multi_upstream_main(void *opaque) {
    multi_upstream_context_t *context = opaque;
    for (size_t i = 0; i < context->expected; ++i) {
        int client;
        do { client = accept(context->listener, NULL, NULL); }
        while (client < 0 && errno == EINTR);
        unsigned char byte = 0u;
        ssize_t amount;
        do { amount = client >= 0 ? recv(client, &byte, 1u, 0) : -1; }
        while (amount < 0 && errno == EINTR);
        if (amount != 1 || !send_all(client, &byte, 1u)) context->failed = 1;
        if (client >= 0) (void)close(client);
    }
    (void)close(context->listener);
    return NULL;
}

typedef struct connector_client_context {
    maelys_egress_connector_t *connector;
    uint16_t port;
    unsigned char value;
    int failed;
} connector_client_context_t;

typedef struct quota_upstream_context {
    int listener;
    size_t expected;
    uint64_t received;
} quota_upstream_context_t;

static void *quota_upstream_main(void *opaque) {
    quota_upstream_context_t *context = opaque;
    for (size_t i = 0; i < context->expected; ++i) {
        int client;
        do { client = accept(context->listener, NULL, NULL); }
        while (client < 0 && errno == EINTR);
        unsigned char bytes[32];
        for (;;) {
            ssize_t amount;
            do { amount = client >= 0 ? recv(client, bytes, sizeof(bytes), 0) : -1; }
            while (amount < 0 && errno == EINTR);
            if (amount <= 0) break;
            context->received += (uint64_t)amount;
        }
        if (client >= 0) (void)close(client);
    }
    (void)close(context->listener);
    return NULL;
}

typedef struct quota_receipts {
    pthread_mutex_t lock;
    size_t count;
    int saw_total;
    int saw_connection;
    uint64_t maximum_execution_after;
    uint64_t maximum_connection_observed;
} quota_receipts_t;

static void quota_receipt_sink(
    void *opaque, const maelys_egress_receipt_t *receipt) {
    quota_receipts_t *capture = opaque;
    (void)pthread_mutex_lock(&capture->lock);
    ++capture->count;
    maelys_egress_quota_scope_t scope =
        maelys_egress_receipt_quota_scope(receipt);
    if (scope == MAELYS_EGRESS_QUOTA_EXECUTION_BYTES) capture->saw_total = 1;
    if (scope == MAELYS_EGRESS_QUOTA_CONNECTION_BYTES) capture->saw_connection = 1;
    uint64_t total = maelys_egress_receipt_quota_execution_after_bytes(receipt);
    uint64_t stream =
        maelys_egress_receipt_quota_connection_observed_bytes(receipt);
    if (total > capture->maximum_execution_after)
        capture->maximum_execution_after = total;
    if (stream > capture->maximum_connection_observed)
        capture->maximum_connection_observed = stream;
    (void)pthread_mutex_unlock(&capture->lock);
}

static void wait_for_quota_receipts(quota_receipts_t *capture, size_t expected) {
    for (unsigned int attempt = 0u; attempt < 3000u; ++attempt) {
        (void)pthread_mutex_lock(&capture->lock);
        size_t count = capture->count;
        (void)pthread_mutex_unlock(&capture->lock);
        if (count >= expected) return;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&delay, NULL);
    }
}

static void *connector_client_main(void *opaque) {
    connector_client_context_t *context = opaque;
    maelys_egress_session_t *session = NULL;
    char *error = NULL;
    if (maelys_egress_connector_session_open(context->connector, "127.0.0.1",
            context->port, 3000u, &session, &error) != MAELYS_EGRESS_OK ||
        !send_all(maelys_egress_session_fd(session), &context->value, 1u)) {
        context->failed = 1;
    } else {
        unsigned char echoed = 0u;
        if (recv(maelys_egress_session_fd(session), &echoed, 1u, MSG_WAITALL) != 1 ||
            echoed != context->value) context->failed = 1;
    }
    maelys_egress_error_free(error);
    maelys_egress_session_release(session);
    return NULL;
}

typedef struct server_context {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    maelys_egress_policy_t *policy;
    maelys_egress_config_t *config;
    maelys_egress_server_t *server;
    uint16_t port;
    uint16_t admin_port;
    int ready;
    maelys_egress_result_t result;
    char *error;
} server_context_t;

static void *server_main(void *opaque) {
    server_context_t *context = opaque;
    context->result = maelys_egress_server_create(
        context->policy, context->config, &context->server, &context->error);
    (void)pthread_mutex_lock(&context->lock);
    context->port = maelys_egress_server_port(context->server);
    context->admin_port = maelys_egress_server_admin_port(context->server);
    context->ready = 1;
    (void)pthread_cond_signal(&context->condition);
    (void)pthread_mutex_unlock(&context->lock);
    if (context->result == MAELYS_EGRESS_OK) {
        context->result = maelys_egress_server_run(context->server, &context->error);
    }
    maelys_egress_server_destroy(context->server);
    return NULL;
}

static int proxy_connect(uint16_t proxy_port, uint16_t destination_port) {
    int fd = connect_loopback(proxy_port);
    if (fd < 0) return -1;
    char request[512];
    int length = snprintf(request, sizeof(request),
        "CONNECT 127.0.0.1:%u HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
        "Proxy-Authorization: Bearer 0123456789abcdef\r\n\r\n",
        (unsigned int)destination_port, (unsigned int)destination_port);
    char response[1024] = {0};
    if (length <= 0 || !send_all(fd, request, (size_t)length) ||
        !receive_header(fd, response, sizeof(response))) {
        (void)close(fd); return -1;
    }
    if (strncmp(response, "HTTP/1.1 200", 12u) != 0) {
        int denied = strncmp(response, "HTTP/1.1 403", 12u) == 0;
        (void)close(fd); return denied ? -2 : -1;
    }
    return fd;
}

/* The canonical receipt encoding is public evidence: attestors sign it and
 * audit journals embed it, so its exact bytes are pinned here. */
static void test_receipt_canonical(void) {
    maelys_egress_receipt_t receipt;
    memset(&receipt, 0, sizeof(receipt));
    receipt.id = 7u;
    receipt.protocol = MAELYS_EGRESS_PROTOCOL_HTTP_CONNECT;
    (void)snprintf(receipt.host, sizeof(receipt.host), "%s", "example.test");
    receipt.port = 443u;
    receipt.result = MAELYS_EGRESS_OK;
    receipt.started_unix_ms = 1000u;
    receipt.duration_ms = 5u;
    receipt.bytes_from_client = 10u;
    receipt.bytes_to_client = 20u;
    (void)snprintf(receipt.policy_digest_hex, sizeof(receipt.policy_digest_hex),
                   "%s", "abcd");
    (void)snprintf(receipt.invocation_id, sizeof(receipt.invocation_id), "%s", "inv-1");
    receipt.tls_sni_verified = 1;
    (void)snprintf(receipt.principal, sizeof(receipt.principal), "%s", "maelys");
    receipt.policy_generation = 3u;
    receipt.quota_scope = MAELYS_EGRESS_QUOTA_CONNECTION_BYTES;
    receipt.quota_connection_max_bytes = 100u;
    receipt.quota_execution_max_bytes = 200u;
    receipt.quota_connection_observed_bytes = 30u;
    receipt.quota_execution_before_bytes = 40u;
    receipt.quota_execution_after_bytes = 70u;
    char canonical[512];
    int length = egress_receipt_canonical(&receipt, canonical, sizeof(canonical));
    static const char expected[] =
        "id=7|principal=maelys|invocation=inv-1|protocol=%d|host=example.test|port=443|"
        "result=%d|started=1000|duration=5|from=10|to=20|policy=abcd|generation=3|sni=1|"
        "quota-scope=%d|quota-connection-max=100|quota-execution-max=200|"
        "quota-connection-observed=30|quota-execution-before=40|quota-execution-after=70";
    char rendered[512];
    int expected_length = snprintf(rendered, sizeof(rendered), expected,
        (int)MAELYS_EGRESS_PROTOCOL_HTTP_CONNECT, (int)MAELYS_EGRESS_OK,
        (int)MAELYS_EGRESS_QUOTA_CONNECTION_BYTES);
    CHECK(length == expected_length && strcmp(canonical, rendered) == 0);
    char small[16];
    CHECK(egress_receipt_canonical(&receipt, small, sizeof(small)) == -1);
    CHECK(egress_receipt_canonical(NULL, canonical, sizeof(canonical)) == -1);
}

static void test_operations(void) {
    uint16_t upstream_port = 0u;
    int upstream_listener = listener_create(&upstream_port);
    CHECK(upstream_listener >= 0);
    upstream_context_t upstream = {.listener = upstream_listener};
    pthread_t upstream_thread;
    CHECK(pthread_create(&upstream_thread, NULL, upstream_main, &upstream) == 0);

    char audit_path[] = "/tmp/maelys-egress-audit-XXXXXX";
    int audit_fd = mkstemp(audit_path);
    CHECK(audit_fd >= 0);
    if (audit_fd >= 0) { CHECK(fchmod(audit_fd, 0600) == 0); (void)close(audit_fd); }
    static const unsigned char audit_key[] = "0123456789abcdef0123456789abcdef";
    maelys_egress_audit_t *audit = NULL;
    maelys_egress_attestor_t *attestor = NULL;
    maelys_egress_policy_t *policy = NULL;
    maelys_egress_policy_t *replacement = NULL;
    maelys_egress_config_t *config = NULL;
    char *error = NULL;
    CHECK(maelys_egress_audit_file_create(audit_path, audit_key,
        sizeof(audit_key) - 1u, "test-key", &audit, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_attestor_create("test-attestor", "test-signing-key", 4u,
        test_attest, NULL, NULL, &attestor, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "127.0.0.1", upstream_port, 1,
        &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_create(&replacement, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(replacement, "127.0.0.1", 1u, 1,
        &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(replacement, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_authentication(config, "maelys",
        "0123456789abcdef", &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_principal_quota(config, "maelys", 1u, 1024u,
        &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_admin_listen(config, "127.0.0.1", 0u,
        &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_audit(config, audit, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_receipt_attestor(config, attestor, &error) ==
          MAELYS_EGRESS_OK);
    maelys_egress_attestor_release(attestor);
    attestor = NULL;

    server_context_t server = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .policy = policy,
        .config = config
    };
    pthread_t server_thread;
    CHECK(pthread_create(&server_thread, NULL, server_main, &server) == 0);
    (void)pthread_mutex_lock(&server.lock);
    while (!server.ready) (void)pthread_cond_wait(&server.condition, &server.lock);
    (void)pthread_mutex_unlock(&server.lock);
    CHECK(server.result == MAELYS_EGRESS_OK && server.port != 0u && server.admin_port != 0u);

    maelys_egress_connector_t *connector = NULL;
    maelys_egress_connector_t *rejected_connector = NULL;
    CHECK(maelys_egress_server_connector_create(server.server, "maelys",
        "wrong-credential-value", &rejected_connector, &error) ==
        MAELYS_EGRESS_ERR_DENIED);
    CHECK(rejected_connector == NULL);
    maelys_egress_error_free(error); error = NULL;
    CHECK(maelys_egress_server_connector_create(server.server, "maelys",
        "0123456789abcdef", &connector, &error) == MAELYS_EGRESS_OK);
    maelys_egress_session_t *session = NULL;
    CHECK(maelys_egress_connector_session_open(connector, "127.0.0.1",
        upstream_port, 3000u, &session, &error) == MAELYS_EGRESS_OK);
    CHECK(session != NULL && maelys_egress_session_fd(session) >= 0);
    int first = -1;
    CHECK(maelys_egress_session_take_fd(session, &first, &error) == MAELYS_EGRESS_OK);
    CHECK(first >= 0 && maelys_egress_session_fd(session) == -1);
    maelys_egress_session_release(session);
    session = NULL;
    CHECK(maelys_egress_connector_session_open(connector, "127.0.0.1",
        upstream_port, 3000u, &session, &error) == MAELYS_EGRESS_ERR_DENIED);
    CHECK(session == NULL);
    maelys_egress_error_free(error); error = NULL;
    uint64_t generation = 0u;
    CHECK(maelys_egress_server_replace_policy(server.server, replacement,
        &generation, &error) == MAELYS_EGRESS_OK);
    CHECK(generation == 2u);
    CHECK(proxy_connect(server.port, upstream_port) == -2);
    CHECK(send_all(first, "ping", 4u));
    char pong[4];
    CHECK(recv(first, pong, sizeof(pong), MSG_WAITALL) == 4 &&
          memcmp(pong, "ping", 4u) == 0);
    (void)close(first);
    maelys_egress_connector_release(connector);

    int admin = connect_loopback(server.admin_port);
    CHECK(admin >= 0);
    static const char metrics_request[] =
        "GET /metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    CHECK(send_all(admin, metrics_request, sizeof(metrics_request) - 1u));
    char metrics[4096] = {0};
    size_t used = 0u;
    while (used + 1u < sizeof(metrics)) {
        ssize_t amount = recv(admin, metrics + used, sizeof(metrics) - used - 1u, 0);
        if (amount <= 0) break;
        used += (size_t)amount;
    }
    CHECK(strstr(metrics, "HTTP/1.1 200 OK") != NULL);
    CHECK(strstr(metrics, "maelys_egress_policy_generation 2") != NULL);
    (void)close(admin);

    maelys_egress_metrics_t *snapshot = NULL;
    CHECK(maelys_egress_server_metrics_snapshot(server.server, &snapshot, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(maelys_egress_metrics_policy_generation(snapshot) == 2u);
    CHECK(maelys_egress_metrics_quota_denials(snapshot) >= 1u);
    maelys_egress_metrics_destroy(snapshot);
    CHECK(maelys_egress_server_stop(server.server) == MAELYS_EGRESS_OK);
    CHECK(pthread_join(server_thread, NULL) == 0);
    CHECK(pthread_join(upstream_thread, NULL) == 0 && upstream.failed == 0);
    CHECK(maelys_egress_audit_record_count(audit) >= 3u);
    uint64_t audit_records = maelys_egress_audit_record_count(audit);
    char chain[65];
    CHECK(maelys_egress_audit_chain_copy(audit, chain) == MAELYS_EGRESS_OK);
    CHECK(strlen(chain) == 64u && strspn(chain, "0") != 64u);
    struct stat audit_status;
    CHECK(stat(audit_path, &audit_status) == 0 && audit_status.st_size > 0);
    int inspect_fd = open(audit_path, O_RDONLY);
    char inspect[4096] = {0};
    ssize_t inspect_length = inspect_fd >= 0 ?
        read(inspect_fd, inspect, sizeof(inspect) - 1u) : -1;
    if (inspect_fd >= 0) (void)close(inspect_fd);
    CHECK(inspect_length > 0 && strstr(inspect, "attestor=test-attestor") != NULL);
    CHECK(inspect_length > 0 && strstr(inspect, "attestation=4d4145") != NULL);
    CHECK(inspect_length > 0 && strstr(inspect, "protocol=4") != NULL);

    maelys_egress_error_free(error);
    maelys_egress_config_destroy(config);
    maelys_egress_audit_release(audit);
    audit = NULL;
    error = NULL;
    maelys_egress_result_t resume_result = maelys_egress_audit_file_create(
        audit_path, audit_key, sizeof(audit_key) - 1u,
        "test-key", &audit, &error);
    if (resume_result != MAELYS_EGRESS_OK) {
        fprintf(stderr, "audit resume: %s\n", error ? error : "no diagnostic");
    }
    CHECK(resume_result == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_audit_record_count(audit) == audit_records);
    char resumed_chain[65];
    CHECK(maelys_egress_audit_chain_copy(audit, resumed_chain) == MAELYS_EGRESS_OK);
    CHECK(strcmp(chain, resumed_chain) == 0);
    maelys_egress_audit_release(audit);
    audit = NULL;
    int corrupt_fd = open(audit_path, O_RDWR);
    CHECK(corrupt_fd >= 0);
    char audit_bytes[65536];
    ssize_t audit_length = corrupt_fd >= 0 ?
        read(corrupt_fd, audit_bytes, sizeof(audit_bytes) - 1u) : -1;
    CHECK(audit_length > 0);
    if (audit_length > 0) {
        audit_bytes[audit_length] = '\0';
        char *mac = strstr(audit_bytes, "\"mac\":\"");
        CHECK(mac != NULL);
        if (mac) {
            off_t offset = (off_t)(mac - audit_bytes + 7);
            char changed = mac[7] == '0' ? '1' : '0';
            CHECK(pwrite(corrupt_fd, &changed, 1u, offset) == 1);
        }
    }
    if (corrupt_fd >= 0) (void)close(corrupt_fd);
    maelys_egress_audit_t *corrupt = NULL;
    maelys_egress_error_free(error); error = NULL;
    CHECK(maelys_egress_audit_file_create(audit_path, audit_key,
        sizeof(audit_key) - 1u, "test-key", &corrupt, &error) ==
        MAELYS_EGRESS_ERR_CRYPTO);
    CHECK(corrupt == NULL && error != NULL);
    maelys_egress_error_free(error);
    maelys_egress_policy_destroy(replacement);
    maelys_egress_policy_destroy(policy);
    CHECK(unlink(audit_path) == 0);
    (void)pthread_cond_destroy(&server.condition);
    (void)pthread_mutex_destroy(&server.lock);
}

static void test_connector_guard_and_timeout(void) {
    uint16_t upstream_port = 0u;
    int listener = listener_create(&upstream_port);
    CHECK(listener >= 0);
    guard_upstream_context_t upstream = {.listener = listener};
    pthread_t upstream_thread;
    CHECK(pthread_create(&upstream_thread, NULL, guard_upstream_main, &upstream) == 0);

    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    char *error = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "localhost", upstream_port, 1,
        &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_require_tls_sni(policy, "localhost", upstream_port,
        &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_authentication(config, "native",
        "0123456789abcdef", &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_native_only(config, 1, &error) ==
          MAELYS_EGRESS_OK);
    connector_receipts_t receipts = {0};
    maelys_egress_config_set_receipt_sink(config, connector_receipt_sink, &receipts);
    server_context_t server = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .policy = policy,
        .config = config
    };
    pthread_t server_thread;
    CHECK(pthread_create(&server_thread, NULL, server_main, &server) == 0);
    (void)pthread_mutex_lock(&server.lock);
    while (!server.ready) (void)pthread_cond_wait(&server.condition, &server.lock);
    (void)pthread_mutex_unlock(&server.lock);
    CHECK(server.result == MAELYS_EGRESS_OK && server.port == 0u);
    for (unsigned int attempt = 0u;
         attempt < 1000u && !maelys_egress_server_is_running(server.server);
         ++attempt) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&delay, NULL);
    }
    CHECK(maelys_egress_server_is_running(server.server));

    maelys_egress_connector_t *connector = NULL;
    maelys_egress_session_t *session = NULL;
    CHECK(maelys_egress_server_connector_create(server.server, "native",
        "0123456789abcdef", &connector, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_connector_session_open(connector, "LOCALHOST", upstream_port,
        1000u, &session, &error) == MAELYS_EGRESS_ERR_ARGUMENT);
    maelys_egress_error_free(error); error = NULL;
    /* A numeric form a resolver reads as an address is not a canonical host
     * for a native session either. */
    CHECK(maelys_egress_connector_session_open(connector, "127.1", upstream_port,
        1000u, &session, &error) == MAELYS_EGRESS_ERR_ARGUMENT);
    maelys_egress_error_free(error); error = NULL;
    CHECK(maelys_egress_connector_session_open(connector, "010.0.0.1", upstream_port,
        1000u, &session, &error) == MAELYS_EGRESS_ERR_ARGUMENT);
    maelys_egress_error_free(error); error = NULL;
    CHECK(maelys_egress_connector_session_open(connector, "localhost",
        (uint16_t)(upstream_port == UINT16_MAX ? upstream_port - 1u : upstream_port + 1u),
        1000u, &session, &error) == MAELYS_EGRESS_ERR_DENIED);
    maelys_egress_error_free(error); error = NULL;
    CHECK(maelys_egress_connector_session_open(connector, "localhost", upstream_port,
        3000u, &session, &error) == MAELYS_EGRESS_OK);
    int type = 0;
    socklen_t type_length = sizeof(type);
    struct sockaddr_storage local_address;
    socklen_t local_length = sizeof(local_address);
    CHECK(getsockopt(maelys_egress_session_fd(session), SOL_SOCKET, SO_TYPE,
        &type, &type_length) == 0 && type == SOCK_STREAM);
    CHECK(getsockname(maelys_egress_session_fd(session),
        (struct sockaddr *)&local_address, &local_length) == 0 &&
        local_address.ss_family == AF_INET);
    CHECK(send_all(maelys_egress_session_fd(session), "not-tls", 7u));
    char response;
    CHECK(recv(maelys_egress_session_fd(session), &response, 1u, 0) <= 0);
    maelys_egress_session_release(session);
    maelys_egress_connector_release(connector);
    CHECK(maelys_egress_server_stop(server.server) == MAELYS_EGRESS_OK);
    CHECK(pthread_join(server_thread, NULL) == 0);
    CHECK(pthread_join(upstream_thread, NULL) == 0);
    CHECK(!upstream.received_application_bytes);
    CHECK(receipts.saw_connector && receipts.saw_denied &&
          receipts.guarded_client_bytes >= 7u);
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    maelys_egress_error_free(error);
    (void)pthread_cond_destroy(&server.condition);
    (void)pthread_mutex_destroy(&server.lock);

    policy = NULL; config = NULL; error = NULL; connector = NULL; session = NULL;
    maelys_egress_server_t *idle_server = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "127.0.0.1", 9u, 1,
        &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_authentication(config, "idle",
        "0123456789abcdef", &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_server_create(policy, config, &idle_server, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(maelys_egress_server_connector_create(idle_server, "idle",
        "0123456789abcdef", &connector, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_connector_session_open(connector, "127.0.0.1", 9u,
        20u, &session, &error) == MAELYS_EGRESS_ERR_TIMEOUT);
    CHECK(session == NULL);
    maelys_egress_error_free(error);
    maelys_egress_server_destroy(idle_server);
    error = NULL;
    CHECK(maelys_egress_connector_session_open(connector, "127.0.0.1", 9u,
        20u, &session, &error) == MAELYS_EGRESS_ERR_STATE);
    CHECK(session == NULL);
    maelys_egress_error_free(error);
    maelys_egress_connector_release(connector);
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
}

static void test_connector_concurrency(void) {
    uint16_t upstream_port = 0u;
    int listener = listener_create(&upstream_port);
    CHECK(listener >= 0);
    multi_upstream_context_t upstream = {
        .listener = listener,
        .expected = CONNECTOR_CONCURRENCY
    };
    pthread_t upstream_thread;
    CHECK(pthread_create(&upstream_thread, NULL, multi_upstream_main, &upstream) == 0);
    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    char *error = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "127.0.0.1", upstream_port, 1,
        &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_authentication(config, "parallel",
        "0123456789abcdef", &error) == MAELYS_EGRESS_OK);
    server_context_t server = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .policy = policy,
        .config = config
    };
    pthread_t server_thread;
    CHECK(pthread_create(&server_thread, NULL, server_main, &server) == 0);
    (void)pthread_mutex_lock(&server.lock);
    while (!server.ready) (void)pthread_cond_wait(&server.condition, &server.lock);
    (void)pthread_mutex_unlock(&server.lock);
    maelys_egress_connector_t *connector = NULL;
    CHECK(maelys_egress_server_connector_create(server.server, "parallel",
        "0123456789abcdef", &connector, &error) == MAELYS_EGRESS_OK);
    pthread_t clients[CONNECTOR_CONCURRENCY];
    connector_client_context_t client_contexts[CONNECTOR_CONCURRENCY];
    memset(client_contexts, 0, sizeof(client_contexts));
    for (size_t i = 0; i < CONNECTOR_CONCURRENCY; ++i) {
        client_contexts[i].connector = connector;
        client_contexts[i].port = upstream_port;
        client_contexts[i].value = (unsigned char)(i + 1u);
        CHECK(pthread_create(&clients[i], NULL, connector_client_main,
                             &client_contexts[i]) == 0);
    }
    for (size_t i = 0; i < CONNECTOR_CONCURRENCY; ++i) {
        CHECK(pthread_join(clients[i], NULL) == 0);
        CHECK(!client_contexts[i].failed);
    }
    maelys_egress_connector_release(connector);
    CHECK(maelys_egress_server_stop(server.server) == MAELYS_EGRESS_OK);
    CHECK(pthread_join(server_thread, NULL) == 0);
    CHECK(pthread_join(upstream_thread, NULL) == 0 && !upstream.failed);
    maelys_egress_error_free(error);
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    (void)pthread_cond_destroy(&server.condition);
    (void)pthread_mutex_destroy(&server.lock);
}

static void test_cumulative_quota(void) {
    uint16_t upstream_port = 0u;
    int listener = listener_create(&upstream_port);
    CHECK(listener >= 0);
    quota_upstream_context_t upstream = {.listener = listener, .expected = 3u};
    pthread_t upstream_thread;
    CHECK(pthread_create(&upstream_thread, NULL, quota_upstream_main, &upstream) == 0);
    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    char *error = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "127.0.0.1", upstream_port, 1,
        &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_native_only(config, 1, &error) ==
        MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_add_principal(config, "total",
        "0123456789abcdef", "quota-total", &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_principal_quota_v2(config, "total", 4u,
        16u, 12u, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_add_principal(config, "stream",
        "fedcba9876543210", "quota-stream", &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_principal_quota_v2(config, "stream", 4u,
        4u, 100u, &error) == MAELYS_EGRESS_OK);
    quota_receipts_t receipts = {.lock = PTHREAD_MUTEX_INITIALIZER};
    maelys_egress_config_set_receipt_sink(config, quota_receipt_sink, &receipts);
    server_context_t server = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .policy = policy,
        .config = config
    };
    pthread_t server_thread;
    CHECK(pthread_create(&server_thread, NULL, server_main, &server) == 0);
    (void)pthread_mutex_lock(&server.lock);
    while (!server.ready) (void)pthread_cond_wait(&server.condition, &server.lock);
    (void)pthread_mutex_unlock(&server.lock);
    maelys_egress_connector_t *total = NULL;
    maelys_egress_connector_t *stream = NULL;
    CHECK(maelys_egress_server_connector_create(server.server, "total",
        "0123456789abcdef", &total, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_server_connector_create(server.server, "stream",
        "fedcba9876543210", &stream, &error) == MAELYS_EGRESS_OK);
    static const unsigned char payload[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    maelys_egress_session_t *first = NULL;
    maelys_egress_session_t *second = NULL;
    CHECK(maelys_egress_connector_session_open(total, "127.0.0.1", upstream_port,
        3000u, &first, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_connector_session_open(total, "127.0.0.1", upstream_port,
        3000u, &second, &error) == MAELYS_EGRESS_OK);
    CHECK(send_all(maelys_egress_session_fd(first), payload, 7u));
    CHECK(send_all(maelys_egress_session_fd(second), payload, 7u));
    unsigned char ignored;
    CHECK(recv(maelys_egress_session_fd(second), &ignored, 1u, 0) <= 0);
    maelys_egress_session_release(first);
    maelys_egress_session_release(second);
    wait_for_quota_receipts(&receipts, 2u);
    maelys_egress_session_t *session = NULL;
    CHECK(maelys_egress_connector_session_open(total, "127.0.0.1", upstream_port,
        3000u, &session, &error) == MAELYS_EGRESS_ERR_DENIED);
    CHECK(session == NULL);
    maelys_egress_error_free(error);
    error = NULL;
    wait_for_quota_receipts(&receipts, 3u);
    CHECK(maelys_egress_connector_session_open(stream, "127.0.0.1", upstream_port,
        3000u, &session, &error) == MAELYS_EGRESS_OK);
    CHECK(send_all(maelys_egress_session_fd(session), payload, sizeof(payload)));
    CHECK(recv(maelys_egress_session_fd(session), &ignored, 1u, 0) <= 0);
    maelys_egress_session_release(session);
    wait_for_quota_receipts(&receipts, 4u);
    (void)pthread_mutex_lock(&receipts.lock);
    CHECK(receipts.count >= 4u && receipts.saw_total && receipts.saw_connection);
    CHECK(receipts.maximum_execution_after <= 12u);
    CHECK(receipts.maximum_connection_observed <= 12u);
    (void)pthread_mutex_unlock(&receipts.lock);
    maelys_egress_connector_release(total);
    maelys_egress_connector_release(stream);
    CHECK(maelys_egress_server_stop(server.server) == MAELYS_EGRESS_OK);
    CHECK(pthread_join(server_thread, NULL) == 0);
    CHECK(pthread_join(upstream_thread, NULL) == 0);
    CHECK(upstream.received <= 16u);
    maelys_egress_error_free(error);
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    (void)pthread_mutex_destroy(&receipts.lock);
    (void)pthread_cond_destroy(&server.condition);
    (void)pthread_mutex_destroy(&server.lock);
}


/*
 * The mediated-connection channel, both ends in one process: the server
 * thread of libmaelys_egress on one side of the pair, libmaelys_egress_client
 * on the other, and raw datagrams where the client would never produce the
 * case. Descriptor counts before and after each exchange hold the ownership
 * decisions of the contract.
 */
static int open_descriptors(void) {
    int count = 0;
    for (int fd = 0; fd < 128; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    }
    return count;
}

static void channel_send_raw(int channel_fd, const unsigned char *bytes, size_t length) {
    CHECK(send(channel_fd, bytes, length, 0) == (ssize_t)length);
}

/* A request with descriptors attached, which no client of the contract
 * sends: on macOS the kernel installs them in the receiver whether or not
 * it asked for control data. */
static void channel_send_with_rights(int channel_fd, const unsigned char *bytes, size_t length,
                                     int descriptor, size_t count) {
    int rights[64];
    CHECK(count <= sizeof(rights) / sizeof(rights[0]));
    for (size_t i = 0; i < count; ++i) rights[i] = descriptor;
    struct iovec iov = {.iov_base = (void *)bytes, .iov_len = length};
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int) * 64u)];
    } control;
    memset(&control, 0, sizeof(control));
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1u;
    message.msg_control = control.bytes;
    message.msg_controllen = (socklen_t)CMSG_SPACE(sizeof(int) * count);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = (socklen_t)CMSG_LEN(sizeof(int) * count);
    memcpy(CMSG_DATA(header), rights, sizeof(int) * count);
    CHECK(sendmsg(channel_fd, &message, 0) == (ssize_t)length);
}

/* Reads one raw response: returns the status byte, sets *out_version and
 * *out_rights to what arrived, closing every descriptor that came with it. */
static int channel_read_raw(int channel_fd, unsigned int *out_version, size_t *out_rights) {
    unsigned char response[MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE + 8u];
    struct iovec iov = {.iov_base = response, .iov_len = sizeof(response)};
    unsigned char control[CMSG_SPACE(sizeof(int) * 4u)];
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    memset(control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1u;
    message.msg_control = control;
    message.msg_controllen = (socklen_t)sizeof(control);
    ssize_t received;
    do { received = recvmsg(channel_fd, &message, 0); } while (received < 0 && errno == EINTR);
    CHECK(received == (ssize_t)MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE);
    *out_rights = 0u;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
        size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int descriptor = -1;
            memcpy(&descriptor, CMSG_DATA(header) + i * sizeof(int), sizeof(descriptor));
            (void)close(descriptor);
            ++*out_rights;
        }
    }
    maelys_egress_channel_response_t decoded;
    CHECK(maelys_egress_channel_decode_response(response, (size_t)received, &decoded, NULL) == 1);
    *out_version = decoded.version;
    return decoded.status;
}

/* An echo upstream that serves connections one after another until told
 * to stop: the main thread sets stop, then connects once to unblock accept. */
typedef struct channel_upstream_context {
    int listener;
    atomic_int stop;
} channel_upstream_context_t;

/* One thread per accepted connection: a stream left open by the test must
 * not hold up the end of another, since Egress waits for the upstream's
 * side to close before it ends a half-closed session. */
static void *channel_echo_main(void *opaque) {
    int client = (int)(intptr_t)opaque;
    char bytes[64];
    for (;;) {
        ssize_t amount = recv(client, bytes, sizeof(bytes), 0);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0 || !send_all(client, bytes, (size_t)amount)) break;
    }
    (void)close(client);
    return NULL;
}

static void *channel_upstream_main(void *opaque) {
    channel_upstream_context_t *context = opaque;
    for (;;) {
        int client;
        do { client = accept(context->listener, NULL, NULL); }
        while (client < 0 && errno == EINTR);
        if (client < 0 || atomic_load(&context->stop)) {
            if (client >= 0) (void)close(client);
            return NULL;
        }
        pthread_t echo;
        if (pthread_create(&echo, NULL, channel_echo_main, (void *)(intptr_t)client) != 0) {
            (void)close(client);
        } else {
            (void)pthread_detach(echo);
        }
    }
}

/* Descriptors close a moment after the peer that made them go away: the
 * relay's ends and the upstream's accepted socket follow the stream. */
static int descriptors_settle_at(int expected) {
    for (unsigned int attempt = 0u; attempt < 3000u; ++attempt) {
        if (open_descriptors() == expected) return 1;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&delay, NULL);
    }
    return open_descriptors() == expected;
}

static uint64_t active_sessions(maelys_egress_server_t *server) {
    maelys_egress_metrics_t *metrics = NULL;
    char *error = NULL;
    CHECK(maelys_egress_server_metrics_snapshot(server, &metrics, &error) == MAELYS_EGRESS_OK);
    uint64_t active = metrics ? maelys_egress_metrics_active(metrics) : UINT64_MAX;
    maelys_egress_metrics_destroy(metrics);
    maelys_egress_error_free(error);
    return active;
}

static void wait_until_idle(maelys_egress_server_t *server) {
    for (unsigned int attempt = 0u; attempt < 2000u && active_sessions(server) != 0u; ++attempt) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&delay, NULL);
    }
}

typedef struct channel_destroy_context {
    maelys_egress_channel_t *channel;
    atomic_int returned;
} channel_destroy_context_t;

static void *channel_destroy_main(void *opaque) {
    channel_destroy_context_t *context = opaque;
    maelys_egress_channel_destroy(context->channel);
    atomic_store(&context->returned, 1);
    return NULL;
}

/* A confined client is not trusted to obey the one-request rule or to read
 * its answers. Filling the response queue must end the channel and must never
 * strand the supervisor in channel_destroy. */
static void test_channel_response_backpressure(maelys_egress_connector_t *connector) {
    int baseline = open_descriptors();
    maelys_egress_channel_t *channel = NULL;
    int client_fd = -1;
    char *error = NULL;
    CHECK(maelys_egress_channel_create(connector, 100u, &channel, &client_fd, &error) ==
          MAELYS_EGRESS_OK);
    int receive_bytes = 1024;
    CHECK(setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF,
                     &receive_bytes, (socklen_t)sizeof(receive_bytes)) == 0);
    int flags = fcntl(client_fd, F_GETFL);
    CHECK(flags >= 0 && fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == 0);
    int peer_closed = 0;
    size_t sent = 0u;
    for (unsigned int attempt = 0u; attempt < 5000u && !peer_closed; ++attempt) {
        ssize_t amount = send(client_fd, "!", 1u, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (amount == 1) {
            ++sent;
            continue;
        }
        if (amount < 0 && (errno == EINTR || errno == EAGAIN ||
                           errno == EWOULDBLOCK || errno == ENOBUFS)) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
            (void)nanosleep(&delay, NULL);
            continue;
        }
        peer_closed = 1;
    }
    CHECK(sent > 0u && peer_closed);

    channel_destroy_context_t destroy = {.channel = channel};
    atomic_init(&destroy.returned, 0);
    pthread_t destroy_thread;
    int created = pthread_create(&destroy_thread, NULL, channel_destroy_main, &destroy);
    CHECK(created == 0);
    int returned_without_client_close = 0;
    if (created == 0) {
        for (unsigned int attempt = 0u; attempt < 300u; ++attempt) {
            if (atomic_load(&destroy.returned)) {
                returned_without_client_close = 1;
                break;
            }
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
            (void)nanosleep(&delay, NULL);
        }
        /* This releases the known-bad implementation before reporting the
         * failure, so a regression cannot hang the rest of the suite. */
        if (!returned_without_client_close) {
            (void)close(client_fd);
            client_fd = -1;
        }
        CHECK(pthread_join(destroy_thread, NULL) == 0);
    } else {
        (void)close(client_fd);
        client_fd = -1;
        maelys_egress_channel_destroy(channel);
    }
    CHECK(returned_without_client_close);
    if (client_fd >= 0) (void)close(client_fd);
    maelys_egress_error_free(error);
    CHECK(descriptors_settle_at(baseline));
}

static int bootstrap_peer(const char *path) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    REQUIRE(strlen(path) < sizeof(address.sun_path)); strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); REQUIRE(fd >= 0);
    REQUIRE(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    fixture_nonblocking(fd);
    return fd;
}

static unsigned bootstrap_read(int peer, int *out_channel) {
    unsigned char response[16]; size_t offset = 0u;
    *out_channel = -1;
    while (offset < sizeof(response)) {
        fixture_readable(peer);
        int passed = -1; size_t count = 0u, received = 0u; unsigned flags = 0u;
        REQUIRE(maelys_sys_fd_stream_receive(peer, response + offset, sizeof(response) - offset,
            &received, &passed, 1u, &count, &flags) == MAELYS_SYS_OK);
        REQUIRE(!flags && received);
        if (count) { REQUIRE(*out_channel < 0); *out_channel = passed; }
        offset += received;
    }
    unsigned status; uint64_t timeout;
    REQUIRE(egress_bootstrap_decode_response(response, sizeof(response), &status, &timeout));
    REQUIRE((status == EGRESS_BOOTSTRAP_OK) == (*out_channel >= 0));
    return status;
}

static void settle_fds(int expected) {
    unsigned stable = 0u;
    for (unsigned i = 0u; i < 3000u; ++i) {
        if (fixture_fd_count() == expected) { if (++stable == 4u) return; }
        else stable = 0u;
        struct timespec delay = {.tv_nsec = 1000000L}; nanosleep(&delay, NULL);
    }
    REQUIRE(fixture_fd_count() == expected);
}

static void test_bootstrap_broker(maelys_egress_connector_t *connector,
    maelys_egress_server_t *server, uint16_t upstream_port) {
    char directory_template[] = "/tmp/egress-broker-XXXXXX";
    REQUIRE(mkdtemp(directory_template));
    char *directory = realpath(directory_template, NULL); REQUIRE(directory);
    char path[104]; REQUIRE(snprintf(path, sizeof(path), "%s/s", directory) > 0);
    int before = fixture_fd_count();
    maelys_egress_channel_broker_t *broker = NULL;
    REQUIRE(maelys_egress_channel_broker_create(connector, path, 2000u, 120u, 1u, &broker, NULL) == MAELYS_EGRESS_OK);
    int idle = fixture_fd_count();
    struct stat st; REQUIRE(lstat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    maelys_egress_channel_broker_t *duplicate = NULL;
    REQUIRE(maelys_egress_channel_broker_create(connector, path, 2000u, 120u, 1u, &duplicate, NULL) == MAELYS_EGRESS_ERR_IO);
    REQUIRE(!duplicate && fixture_fd_count() == idle);
    maelys_egress_client_channel_t *client = NULL, *busy = NULL;
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &client, NULL) == MAELYS_EGRESS_CLIENT_OK);
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &busy, NULL) == MAELYS_EGRESS_CLIENT_ERR_BUSY);
    REQUIRE(!busy);
    int stream = -1;
    REQUIRE(maelys_egress_client_connect(maelys_egress_client_channel_fd(client),
        "localhost", upstream_port, 3000u, &stream, NULL) == MAELYS_EGRESS_CLIENT_OK);
    int denied = -1;
    REQUIRE(maelys_egress_client_connect(maelys_egress_client_channel_fd(client),
        "forbidden.invalid", 443u, 3000u, &denied, NULL) == MAELYS_EGRESS_CLIENT_ERR_DENIED);
    maelys_egress_client_channel_close(client);
    REQUIRE(send_all(stream, "live", 4u)); char echoed[4]; fixture_readable(stream);
    REQUIRE(recv(stream, echoed, sizeof(echoed), 0) == 4 && !memcmp(echoed, "live", 4u));
    close(stream); wait_until_idle(server); settle_fds(idle);

    unsigned char request[8]; egress_bootstrap_request(request);
    for (size_t split = 1u; split <= 8u; ++split) {
        int peer = bootstrap_peer(path);
        REQUIRE(send_all(peer, request, split));
        for (size_t i = split; i < 8u; ++i) REQUIRE(send_all(peer, request + i, 1u));
        int channel;
        REQUIRE(bootstrap_read(peer, &channel) == EGRESS_BOOTSTRAP_OK);
        close(channel); close(peer); settle_fds(idle);
    }
    /* Every fragment is a control boundary. Raw hostile sends bypass System. */
    for (size_t offset = 0u; offset < 8u; ++offset) {
        int peer = bootstrap_peer(path);
        if (offset) REQUIRE(send_all(peer, request, offset));
        fixture_rights(peer, request + offset, 8u - offset, 50u);
        int channel; REQUIRE(bootstrap_read(peer, &channel) == EGRESS_BOOTSTRAP_MALFORMED);
        close(peer); settle_fds(idle);
    }
    for (unsigned mutation = 0u; mutation < 8u; ++mutation) {
        unsigned char bad[8]; memcpy(bad, request, 8u); bad[mutation] ^= 2u;
        int peer = bootstrap_peer(path); REQUIRE(send_all(peer, bad, 8u)); int channel;
        REQUIRE(bootstrap_read(peer, &channel) == (mutation == 4u || mutation == 5u ?
            EGRESS_BOOTSTRAP_UNSUPPORTED : EGRESS_BOOTSTRAP_MALFORMED));
        close(peer); settle_fds(idle);
    }
    for (unsigned attack = 0u; attack < 4u; ++attack) {
#ifndef __APPLE__
        if (attack == 3u) continue;
#endif
        int peer = bootstrap_peer(path); REQUIRE(send_all(peer, request, 8u)); int channel;
        REQUIRE(bootstrap_read(peer, &channel) == EGRESS_BOOTSTRAP_OK);
        if (attack == 0u) REQUIRE(send_all(peer, "x", 1u));
        if (attack == 1u) fixture_rights(peer, "x", 1u, 50u);
        if (attack == 2u) REQUIRE(shutdown(peer, SHUT_WR) == 0);
        if (attack == 3u) fixture_rights(peer, "", 0u, 50u);
        fixture_readable(peer);
        unsigned char byte; size_t n = 0u, count = 0u; unsigned flags = 0u;
        maelys_sys_result_t result = maelys_sys_fd_stream_receive(peer, &byte, 1u, &n, NULL, 0u, &count, &flags);
        REQUIRE(result == MAELYS_SYS_ERR_CLOSED || result == MAELYS_SYS_ERR_RESET);
        close(channel); close(peer); settle_fds(idle);
    }
    /* Pending handshakes count towards capacity and have one absolute deadline. */
    int slow = bootstrap_peer(path); REQUIRE(send_all(slow, request, 1u));
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &busy, NULL) == MAELYS_EGRESS_CLIENT_ERR_BUSY);
    fixture_readable(slow); close(slow); settle_fds(idle);
    /* Early close and response delivery abandonment reclaim channels and slots. */
    for (unsigned i = 0u; i < 32u; ++i) {
        int peer = bootstrap_peer(path); REQUIRE(send_all(peer, request, 8u)); close(peer);
        settle_fds(idle);
    }
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &client, NULL) == MAELYS_EGRESS_CLIENT_OK);
    REQUIRE(maelys_egress_client_connect(maelys_egress_client_channel_fd(client), "localhost",
        upstream_port, 3000u, &stream, NULL) == MAELYS_EGRESS_CLIENT_OK);
    REQUIRE(maelys_egress_channel_broker_destroy(broker, NULL) == MAELYS_EGRESS_OK); broker = NULL;
    REQUIRE(access(path, F_OK) != 0);
    REQUIRE(send_all(stream, "kept", 4u)); fixture_readable(stream);
    REQUIRE(recv(stream, echoed, 4u, 0) == 4 && !memcmp(echoed, "kept", 4u));
    maelys_egress_client_channel_close(client); close(stream); wait_until_idle(server); settle_fds(before);

    /* Group-capability mode: readable/traversable, never writable by clients. */
    REQUIRE(chown(directory, (uid_t)-1, getegid()) == 0);
    REQUIRE(chmod(directory, 02750) == 0);
    REQUIRE(maelys_egress_channel_broker_create(connector, path, 100u, 100u, 1u, &broker, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(lstat(path, &st) == 0 && (st.st_mode & 0777) == 0660);
    struct stat parent; REQUIRE(stat(directory, &parent) == 0 && st.st_gid == parent.st_gid);
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &client, NULL) == MAELYS_EGRESS_CLIENT_OK);
    maelys_egress_client_channel_close(client);
    REQUIRE(maelys_egress_channel_broker_destroy(broker, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(chmod(directory, 02770) == 0);
    REQUIRE(maelys_egress_channel_broker_create(connector, path, 100u, 100u, 1u, &broker, NULL) == MAELYS_EGRESS_ERR_IO);
    REQUIRE(chmod(directory, 0700) == 0);
    REQUIRE(symlink("not-a-socket", path) == 0);
    REQUIRE(maelys_egress_channel_broker_create(connector, path, 100u, 100u, 1u, &broker, NULL) == MAELYS_EGRESS_ERR_IO);
    REQUIRE(unlink(path) == 0);
    REQUIRE(maelys_egress_channel_broker_create(connector, path, 100u, 100u, 1u, &broker, NULL) == MAELYS_EGRESS_OK);
    /* A replaced inode belongs to somebody else and must survive destroy. */
    REQUIRE(unlink(path) == 0); int planted = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600); REQUIRE(planted >= 0); close(planted);
    REQUIRE(maelys_egress_channel_broker_destroy(broker, NULL) == MAELYS_EGRESS_ERR_IO);
    REQUIRE(lstat(path, &st) == 0 && S_ISREG(st.st_mode)); REQUIRE(unlink(path) == 0);
    settle_fds(before); REQUIRE(rmdir(directory) == 0); free(directory);
}


static void test_channel(void) {
    /* Everything this test opens — upstream, server, channels, streams —
     * must be gone when it returns. */
    int baseline = open_descriptors();
    uint16_t upstream_port = 0u;
    channel_upstream_context_t upstream = {.listener = listener_create(&upstream_port)};
    CHECK(upstream.listener >= 0);
    pthread_t upstream_thread;
    CHECK(pthread_create(&upstream_thread, NULL, channel_upstream_main, &upstream) == 0);

    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    char *error = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "localhost", upstream_port, 1, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_authentication(config, "native", "0123456789abcdef",
                                                  &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_native_only(config, 1, &error) == MAELYS_EGRESS_OK);
    connector_receipts_t receipts = {0};
    maelys_egress_config_set_receipt_sink(config, connector_receipt_sink, &receipts);
    server_context_t server = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .policy = policy,
        .config = config
    };
    pthread_t server_thread;
    CHECK(pthread_create(&server_thread, NULL, server_main, &server) == 0);
    (void)pthread_mutex_lock(&server.lock);
    while (!server.ready) (void)pthread_cond_wait(&server.condition, &server.lock);
    (void)pthread_mutex_unlock(&server.lock);
    CHECK(server.result == MAELYS_EGRESS_OK);
    for (unsigned int attempt = 0u;
         attempt < 1000u && !maelys_egress_server_is_running(server.server); ++attempt) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&delay, NULL);
    }
    CHECK(maelys_egress_server_is_running(server.server));

    maelys_egress_connector_t *connector = NULL;
    CHECK(maelys_egress_server_connector_create(server.server, "native", "0123456789abcdef",
                                                &connector, &error) == MAELYS_EGRESS_OK);
    maelys_egress_channel_t *channel = NULL;
    int client_fd = -1;
    /* Arguments: a zero deadline is refused, so is a missing output. */
    CHECK(maelys_egress_channel_create(connector, 0u, &channel, &client_fd, &error) ==
          MAELYS_EGRESS_ERR_ARGUMENT);
    maelys_egress_error_free(error); error = NULL;
    CHECK(maelys_egress_channel_create(connector, 3000u, &channel, NULL, &error) ==
          MAELYS_EGRESS_ERR_ARGUMENT);
    maelys_egress_error_free(error); error = NULL;
    CHECK(maelys_egress_channel_create(connector, 3000u, &channel, &client_fd, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(channel != NULL && client_fd >= 0 && (fcntl(client_fd, F_GETFD) & FD_CLOEXEC));
    int with_channel = open_descriptors();

    /* OK: one stream, relayed through Egress, echoed by the upstream. */
    int stream = -1;
    CHECK(maelys_egress_client_connect(client_fd, "localhost", upstream_port, 5000u, &stream,
                                       &error) == MAELYS_EGRESS_CLIENT_OK);
    CHECK(stream >= 0 && error == NULL);
    CHECK(send_all(stream, "ping", 4u));
    char echoed[4] = {0};
    ssize_t got = recv(stream, echoed, sizeof(echoed), 0);
    CHECK(got == 4 && memcmp(echoed, "ping", 4u) == 0);
    (void)close(stream);
    wait_until_idle(server.server);
    CHECK(descriptors_settle_at(with_channel));

    /* DENIED: a port the policy does not allow. The client end is bound to
     * the principal; nothing in the request could name another. */
    uint16_t other_port = (uint16_t)(upstream_port == UINT16_MAX ? upstream_port - 1u
                                                                : upstream_port + 1u);
    CHECK(maelys_egress_client_connect(client_fd, "localhost", other_port, 5000u, &stream,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_DENIED);
    CHECK(stream == -1 && error != NULL);
    maelys_egress_client_error_free(error); error = NULL;
    /* A host the codec accepts but the connector refuses as not canonical:
     * MALFORMED on the wire, a protocol error for the client. */
    CHECK(maelys_egress_client_connect(client_fd, "LOCALHOST", upstream_port, 5000u, &stream,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    maelys_egress_client_error_free(error); error = NULL;
    CHECK(descriptors_settle_at(with_channel));

    /* Raw datagrams the client never produces. */
    unsigned int version = 0u;
    size_t rights = 0u;
    static const unsigned char empty_host[] = {0x4d, 0x45, 0x43, 0x51, 1u, 1u, 0x01, 0xbb, 0u, 0u};
    channel_send_raw(client_fd, empty_host, sizeof(empty_host));
    CHECK(channel_read_raw(client_fd, &version, &rights) == MAELYS_EGRESS_CHANNEL_MALFORMED);
    CHECK(version == 1u && rights == 0u);
    static const unsigned char version_2[] = {0x4d, 0x45, 0x43, 0x51, 2u, 1u, 0x01, 0xbb, 0u, 1u, 'a'};
    channel_send_raw(client_fd, version_2, sizeof(version_2));
    CHECK(channel_read_raw(client_fd, &version, &rights) == MAELYS_EGRESS_CHANNEL_UNSUPPORTED);
    CHECK(version == 1u && rights == 0u);
    static const unsigned char protocol_2[] = {0x4d, 0x45, 0x43, 0x51, 1u, 2u, 0x01, 0xbb, 0u, 1u, 'a'};
    channel_send_raw(client_fd, protocol_2, sizeof(protocol_2));
    CHECK(channel_read_raw(client_fd, &version, &rights) == MAELYS_EGRESS_CHANNEL_UNSUPPORTED);
    channel_send_raw(client_fd, empty_host, 0u);
    CHECK(channel_read_raw(client_fd, &version, &rights) == MAELYS_EGRESS_CHANNEL_MALFORMED);
    unsigned char oversized[MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE + 20u];
    memset(oversized, 'a', sizeof(oversized));
    memcpy(oversized, empty_host, 10u);
    channel_send_raw(client_fd, oversized, sizeof(oversized));
    CHECK(channel_read_raw(client_fd, &version, &rights) == MAELYS_EGRESS_CHANNEL_MALFORMED);
    CHECK(descriptors_settle_at(with_channel));

    /* A valid request carrying descriptors: refused, and every descriptor
     * the kernel installed in this process is closed again. Fifty of them,
     * enough to see a leak at once; the count after equals the count
     * before, on a kernel that installs them (macOS) as on one that drops
     * them (Linux). */
    {
        unsigned char valid[MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE];
        size_t valid_length = maelys_egress_channel_encode_request(
            "localhost", upstream_port, valid, sizeof(valid));
        CHECK(valid_length > 0u);
        int carried[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, carried) == 0);
        int with_carried = open_descriptors();
        for (int round = 0; round < 4; ++round) {
            channel_send_with_rights(client_fd, valid, valid_length, carried[0], 50u);
            CHECK(channel_read_raw(client_fd, &version, &rights) ==
                  MAELYS_EGRESS_CHANNEL_MALFORMED);
            CHECK(rights == 0u);
        }
        CHECK(descriptors_settle_at(with_carried));
        (void)close(carried[0]);
        (void)close(carried[1]);
        CHECK(descriptors_settle_at(with_channel));
    }

    /* Two requests sent before either answer: answered in order, each with
     * its own stream. The contract promises the sequential case; this pins
     * what the server does with the other. */
    unsigned char request[MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE];
    size_t request_length = maelys_egress_channel_encode_request(
        "localhost", upstream_port, request, sizeof(request));
    CHECK(request_length > 0u);
    channel_send_raw(client_fd, request, request_length);
    channel_send_raw(client_fd, request, request_length);
    CHECK(channel_read_raw(client_fd, &version, &rights) == MAELYS_EGRESS_CHANNEL_OK && rights == 1u);
    CHECK(channel_read_raw(client_fd, &version, &rights) == MAELYS_EGRESS_CHANNEL_OK && rights == 1u);
    wait_until_idle(server.server);
    CHECK(descriptors_settle_at(with_channel));

    /* A stream survives the channel that carried it; the channel's end
     * closes nothing already handed over. */
    CHECK(maelys_egress_client_connect(client_fd, "localhost", upstream_port, 5000u, &stream,
                                       &error) == MAELYS_EGRESS_CLIENT_OK);
    maelys_egress_channel_destroy(channel);
    channel = NULL;
    CHECK(send_all(stream, "still", 5u));
    char still[5] = {0};
    CHECK(recv(stream, still, sizeof(still), 0) == 5 && memcmp(still, "still", 5u) == 0);
    /* And the client end of a destroyed channel answers nothing. */
    int late = -1;
    CHECK(maelys_egress_client_connect(client_fd, "localhost", upstream_port, 100u, &late,
                                       &error) != MAELYS_EGRESS_CLIENT_OK);
    CHECK(late == -1);
    maelys_egress_client_error_free(error); error = NULL;
    (void)close(client_fd);

    /* Undelivered: the client end can no longer receive when the answer
     * comes. Shutting its reading side first makes the kernel refuse the
     * server's datagram every time (a close would race the answer, and a
     * datagram already queued would be collected only later). The server
     * closes its copy, releases the session and ends the channel: nothing
     * stays open or active on this side, and the request reached a session
     * that ends. */
    int before_second = open_descriptors();
    int second_client = -1;
    CHECK(maelys_egress_channel_create(connector, 3000u, &channel, &second_client, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(shutdown(second_client, SHUT_RD) == 0);
    channel_send_raw(second_client, request, request_length);
    wait_until_idle(server.server);
    (void)close(second_client);
    maelys_egress_channel_destroy(channel);
    channel = NULL;
    CHECK(descriptors_settle_at(before_second));

    test_channel_response_backpressure(connector);
    test_bootstrap_broker(connector, server.server, upstream_port);

    /* Stopping the server ends the stream handed over earlier. */
    (void)close(stream);
    maelys_egress_connector_release(connector);
    CHECK(maelys_egress_server_stop(server.server) == MAELYS_EGRESS_OK);
    CHECK(pthread_join(server_thread, NULL) == 0);
    atomic_store(&upstream.stop, 1);
    int wake = connect_loopback(upstream_port);
    CHECK(pthread_join(upstream_thread, NULL) == 0);
    if (wake >= 0) (void)close(wake);
    (void)close(upstream.listener);
    CHECK(descriptors_settle_at(baseline));
    CHECK(receipts.saw_connector && receipts.saw_denied);
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    maelys_egress_error_free(error);
    (void)pthread_cond_destroy(&server.condition);
    (void)pthread_mutex_destroy(&server.lock);
}

/* A server whose owner thread returns from run and then waits to be told
 * before it destroys: the interval in which a stopped server exists. */
typedef struct held_server_context {
    server_context_t server;
    int returned;
    int release;
} held_server_context_t;

static void *held_server_main(void *opaque) {
    held_server_context_t *held = opaque;
    server_context_t *context = &held->server;
    context->result = maelys_egress_server_create(
        context->policy, context->config, &context->server, &context->error);
    (void)pthread_mutex_lock(&context->lock);
    context->port = maelys_egress_server_port(context->server);
    context->ready = 1;
    (void)pthread_cond_broadcast(&context->condition);
    (void)pthread_mutex_unlock(&context->lock);
    if (context->result == MAELYS_EGRESS_OK) {
        context->result = maelys_egress_server_run(context->server, &context->error);
    }
    (void)pthread_mutex_lock(&context->lock);
    held->returned = 1;
    (void)pthread_cond_broadcast(&context->condition);
    while (!held->release) (void)pthread_cond_wait(&context->condition, &context->lock);
    (void)pthread_mutex_unlock(&context->lock);
    maelys_egress_server_destroy(context->server);
    return NULL;
}

/* 1 when the peer of fd has gone within two seconds: an end of stream or a
 * reset, and nothing else. */
static int stream_ended(int fd) {
    struct pollfd item = {.fd = fd, .events = POLLIN};
    int ready;
    do { ready = poll(&item, 1u, 2000); } while (ready < 0 && errno == EINTR);
    if (ready != 1) return 0;
    char byte;
    ssize_t amount;
    do { amount = recv(fd, &byte, 1u, 0); } while (amount < 0 && errno == EINTR);
    return amount == 0 || (amount < 0 && errno == ECONNRESET);
}

/* Stopping is thread-safe and destroying belongs to the owner thread, so a
 * holder of a relayed connection has nothing but the stop to wake it: when
 * run has returned, a native session and a proxied tunnel have both ended,
 * with the server not yet destroyed. */
static void test_stop_ends_relayed_connections(void) {
    int baseline = open_descriptors();
    uint16_t upstream_port = 0u;
    channel_upstream_context_t upstream = {.listener = listener_create(&upstream_port)};
    CHECK(upstream.listener >= 0);
    pthread_t upstream_thread;
    CHECK(pthread_create(&upstream_thread, NULL, channel_upstream_main, &upstream) == 0);

    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    char *error = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "127.0.0.1", upstream_port, 1, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_config_set_authentication(config, "maelys", "0123456789abcdef",
                                                  &error) == MAELYS_EGRESS_OK);
    held_server_context_t held = {
        .server = {.lock = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                   .policy = policy, .config = config}
    };
    server_context_t *server = &held.server;
    pthread_t server_thread;
    CHECK(pthread_create(&server_thread, NULL, held_server_main, &held) == 0);
    (void)pthread_mutex_lock(&server->lock);
    while (!server->ready) (void)pthread_cond_wait(&server->condition, &server->lock);
    (void)pthread_mutex_unlock(&server->lock);
    CHECK(server->result == MAELYS_EGRESS_OK);
    for (unsigned int attempt = 0u;
         attempt < 1000u && !maelys_egress_server_is_running(server->server); ++attempt) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&delay, NULL);
    }
    CHECK(maelys_egress_server_is_running(server->server));

    maelys_egress_connector_t *connector = NULL;
    maelys_egress_session_t *session = NULL;
    CHECK(maelys_egress_server_connector_create(server->server, "maelys", "0123456789abcdef",
                                                &connector, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_connector_session_open(connector, "127.0.0.1", upstream_port, 3000u,
                                               &session, &error) == MAELYS_EGRESS_OK);
    int native = session ? maelys_egress_session_fd(session) : -1;
    int tunnel = proxy_connect(server->port, upstream_port);
    CHECK(native >= 0 && tunnel >= 0);
    char echo[4] = {0};
    CHECK(send_all(native, "ping", 4u) && recv(native, echo, 4u, MSG_WAITALL) == 4 &&
          memcmp(echo, "ping", 4u) == 0);
    CHECK(send_all(tunnel, "pong", 4u) && recv(tunnel, echo, 4u, MSG_WAITALL) == 4 &&
          memcmp(echo, "pong", 4u) == 0);

    CHECK(maelys_egress_server_stop(server->server) == MAELYS_EGRESS_OK);
    (void)pthread_mutex_lock(&server->lock);
    while (!held.returned) (void)pthread_cond_wait(&server->condition, &server->lock);
    (void)pthread_mutex_unlock(&server->lock);
    /* run has returned and destroy has not been called. */
    CHECK(stream_ended(native));
    CHECK(stream_ended(tunnel));

    (void)pthread_mutex_lock(&server->lock);
    held.release = 1;
    (void)pthread_cond_broadcast(&server->condition);
    (void)pthread_mutex_unlock(&server->lock);
    maelys_egress_session_release(session);
    maelys_egress_connector_release(connector);
    if (tunnel >= 0) (void)close(tunnel);
    CHECK(pthread_join(server_thread, NULL) == 0);
    atomic_store(&upstream.stop, 1);
    int wake = connect_loopback(upstream_port);
    CHECK(pthread_join(upstream_thread, NULL) == 0);
    if (wake >= 0) (void)close(wake);
    (void)close(upstream.listener);
    CHECK(descriptors_settle_at(baseline));
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    maelys_egress_error_free(error);
    (void)pthread_cond_destroy(&server->condition);
    (void)pthread_mutex_destroy(&server->lock);
}

int main(void) {
    fixture_limits();
    test_receipt_canonical();
    test_operations();
    test_connector_guard_and_timeout();
    test_connector_concurrency();
    test_cumulative_quota();
    test_channel();
    test_stop_ends_relayed_connections();
    if (failures) return 1;
    puts("all operational checks passed");
    return 0;
}
