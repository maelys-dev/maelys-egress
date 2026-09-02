#include "maelys/egress.h"
#include "src/internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <netinet/in.h>
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

int main(void) {
    test_receipt_canonical();
    test_operations();
    test_connector_guard_and_timeout();
    test_connector_concurrency();
    test_cumulative_quota();
    if (failures) return 1;
    puts("all operational checks passed");
    return 0;
}
