/* SPDX-License-Identifier: MPL-2.0 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "src/internal.h"
#include "maelys/egress_client.h"
#include "maelys/egress_channel.h"
#include "maelys/sys/fdpass.h"
#include "tests/tls_socket_fixture.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

static atomic_int send_fault, send_calls, rights_sent, hold_open, entered, release_open, destroying;
static atomic_int refuse_send, busy_sent;
static atomic_int destroyed;
static int fail_worker;

static maelys_sys_result_t partial_send(int socket_fd, const void *bytes,
    size_t length, int passed_fd, size_t *sent) {
    if (atomic_load(&send_fault)) {
        int call = atomic_fetch_add(&send_calls, 1);
        *sent = 0u;
        if (call == 0) return MAELYS_SYS_ERR_WOULD_BLOCK;
        if (call == 1) { errno = EINTR; return MAELYS_SYS_ERR_OS; }
        length = length ? 1u : 0u;
    }
    maelys_sys_result_t result = maelys_sys_fd_stream_send(socket_fd, bytes, length, passed_fd, sent);
    if (result == MAELYS_SYS_OK && *sent && passed_fd >= 0) atomic_fetch_add(&rights_sent, 1);
    if (result == MAELYS_SYS_OK && *sent == 16u &&
        !memcmp(bytes, "MEBP", 4u) && ((const unsigned char *)bytes)[5] == 2u)
        atomic_store(&busy_sent, 1);
    return result;
}

static maelys_egress_result_t held_open(maelys_egress_connector_t *connector,
    const char *host, uint16_t port, uint64_t timeout,
    maelys_egress_session_t **session, char **error) {
    if (atomic_load(&hold_open)) {
        atomic_store(&entered, 1);
        while (!atomic_load(&release_open)) {
            struct timespec delay = {.tv_nsec = 1000000L}; nanosleep(&delay, NULL);
        }
    }
    return maelys_egress_connector_session_open(connector, host, port, timeout, session, error);
}

/* Exercise the actual production channel and broker, substituting only the
 * I/O boundary and a deterministic pending connector open. No timing-based
 * attempt to fill a remote SYN backlog stands in for a suspended command. */
#define maelys_egress_connector_session_open held_open
#include "src/channel_server.c"
#undef maelys_egress_connector_session_open

static void joining_channel(maelys_egress_channel_t *channel) {
    int present = channel != NULL;
    if (channel && atomic_load(&entered)) atomic_store(&destroying, 1);
    maelys_egress_channel_destroy(channel);
    if (present) atomic_fetch_add(&destroyed, 1);
}
static maelys_sys_result_t failing_worker(const char *name, maelys_sys_thread_fn function,
    void *context, maelys_sys_thread_t **thread) {
    if ((fail_worker == 1 && !strcmp(name, "egress-reaper")) ||
        (fail_worker == 2 && !strcmp(name, "egress-broker"))) {
        *thread = NULL;
        return MAELYS_SYS_ERR_OS;
    }
    return maelys_sys_thread_create(name, function, context, thread);
}
#define maelys_sys_fd_stream_send partial_send
#define maelys_egress_channel_destroy joining_channel
#define maelys_sys_thread_create failing_worker
#include "src/channel_broker.c"
#undef maelys_sys_thread_create
#undef maelys_egress_channel_destroy
#undef maelys_sys_fd_stream_send

static maelys_sys_result_t closed_request_send(int fd, const void *bytes,
    size_t length, int passed, size_t *sent) {
    if (!atomic_load(&refuse_send)) return maelys_sys_fd_stream_send(fd, bytes, length, passed, sent);
    for (unsigned i = 0u; i < 2000u && !atomic_load(&busy_sent); ++i) {
        struct timespec delay = {.tv_nsec = 1000000L}; nanosleep(&delay, NULL);
    }
    REQUIRE(atomic_load(&busy_sent));
    *sent = 0u;
    return MAELYS_SYS_ERR_CLOSED;
}
/* Force the Linux race: the peer has queued BUSY before the request write
 * reports EPIPE. The real response decoder must still return ERR_BUSY. */
#define maelys_sys_fd_stream_send closed_request_send
#include "client/channel_open.c"
#undef maelys_sys_fd_stream_send

typedef struct test_server {
    maelys_egress_policy_t *policy;
    maelys_egress_config_t *config;
    maelys_egress_server_t *server;
    atomic_int ready;
} test_server_t;

static void *run_server(void *opaque) {
    test_server_t *test = opaque;
    REQUIRE(maelys_egress_server_create(test->policy, test->config, &test->server, NULL) == MAELYS_EGRESS_OK);
    atomic_store(&test->ready, 1);
    REQUIRE(maelys_egress_server_run(test->server, NULL) == MAELYS_EGRESS_OK);
    maelys_egress_server_destroy(test->server);
    return NULL;
}

static void wait_flag(atomic_int *flag) {
    for (unsigned i = 0u; i < 2000u && !atomic_load(flag); ++i) {
        struct timespec delay = {.tv_nsec = 1000000L}; nanosleep(&delay, NULL);
    }
    REQUIRE(atomic_load(flag));
}

static maelys_egress_client_channel_t *open_when_available(const char *path) {
    uint64_t deadline = 0u, now = 0u;
    REQUIRE(maelys_sys_deadline_after(2000u, &deadline) == MAELYS_SYS_OK);
    maelys_egress_client_channel_t *client = NULL;
    maelys_egress_client_result_t result;
    do {
        result = maelys_egress_client_channel_open(path, 1000u, &client, NULL);
        if (result != MAELYS_EGRESS_CLIENT_ERR_BUSY) break;
        struct timespec delay = {.tv_nsec = 1000000L}; nanosleep(&delay, NULL);
        REQUIRE(maelys_sys_monotonic_ms(&now) == MAELYS_SYS_OK);
    } while (now < deadline);
    REQUIRE(result == MAELYS_EGRESS_CLIENT_OK && client);
    return client;
}

int main(void) {
    fixture_limits(); int baseline = fixture_fd_count();
    test_server_t test = {0}; atomic_init(&test.ready, 0);
    REQUIRE(maelys_egress_policy_create(&test.policy, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(maelys_egress_policy_allow_tcp(test.policy, "127.0.0.1", 9u, 1, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(maelys_egress_policy_seal(test.policy, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(maelys_egress_config_create(&test.config, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(maelys_egress_config_set_authentication(test.config, "native", "0123456789abcdef", NULL) == MAELYS_EGRESS_OK);
    pthread_t thread; REQUIRE(pthread_create(&thread, NULL, run_server, &test) == 0); wait_flag(&test.ready);
    while (!maelys_egress_server_is_running(test.server)) {
        struct timespec delay = {.tv_nsec = 1000000L}; nanosleep(&delay, NULL);
    }
    maelys_egress_connector_t *connector = NULL;
    REQUIRE(maelys_egress_server_connector_create(test.server, "native", "0123456789abcdef", &connector, NULL) == MAELYS_EGRESS_OK);
    char template[] = "/tmp/egress-broker-faults-XXXXXX";
    REQUIRE(mkdtemp(template)); char *directory = realpath(template, NULL); REQUIRE(directory);
    char path[104]; REQUIRE(snprintf(path, sizeof(path), "%s/s", directory) > 0);
    maelys_egress_channel_broker_t *broker = NULL;
    int before_broker = fixture_fd_count();
    for (fail_worker = 1; fail_worker <= 2; ++fail_worker) {
        REQUIRE(maelys_egress_channel_broker_create(connector, path, 1000u, 500u, 1u, &broker, NULL) == MAELYS_EGRESS_ERR_IO);
        REQUIRE(!broker && access(path, F_OK) != 0 && fixture_fd_count() == before_broker);
    }
    fail_worker = 0;
    REQUIRE(maelys_egress_channel_broker_create(connector, path, 1000u, 500u, 1u, &broker, NULL) == MAELYS_EGRESS_OK);
    atomic_store(&send_fault, 1);
    maelys_egress_client_channel_t *client = NULL;
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &client, NULL) == MAELYS_EGRESS_CLIENT_OK);
    REQUIRE(atomic_load(&rights_sent) == 1 && atomic_load(&send_calls) == 18);
    atomic_store(&send_fault, 0);
    atomic_store(&refuse_send, 1);
    maelys_egress_client_channel_t *busy = NULL;
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &busy, NULL) == MAELYS_EGRESS_CLIENT_ERR_BUSY);
    REQUIRE(!busy);
    atomic_store(&refuse_send, 0);
    maelys_egress_client_channel_close(client);
    REQUIRE(maelys_egress_channel_broker_destroy(broker, NULL) == MAELYS_EGRESS_OK);

    /* Three slots: one held in destruction, one healthy lease, one partial
     * handshake. A blocked reaper must neither stall the reactor nor release
     * its capacity early. Keep the first open suspended until all proofs end. */
    atomic_store(&destroyed, 0);
    REQUIRE(maelys_egress_channel_broker_create(connector, path, 1000u, 1000u, 3u, &broker, NULL) == MAELYS_EGRESS_OK);
    client = open_when_available(path);
    atomic_store(&hold_open, 1);
    unsigned char request[MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE];
    size_t length = maelys_egress_channel_encode_request("held.invalid", 443u, request, sizeof(request));
    REQUIRE(maelys_sys_fd_send(maelys_egress_client_channel_fd(client), request, length, -1) == MAELYS_SYS_OK);
    wait_flag(&entered);
    maelys_egress_client_channel_close(client);
    wait_flag(&destroying);

    maelys_egress_client_channel_t *healthy = NULL, *third = NULL;
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &healthy, NULL) == MAELYS_EGRESS_CLIENT_OK);
    int slow = socket(AF_UNIX, SOCK_STREAM, 0); REQUIRE(slow >= 0);
    struct sockaddr_un unix_address = {.sun_family = AF_UNIX};
    strcpy(unix_address.sun_path, path);
    REQUIRE(connect(slow, (struct sockaddr *)&unix_address, sizeof(unix_address)) == 0);
    REQUIRE(send(slow, "M", 1u, 0) == 1);
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &busy, NULL) == MAELYS_EGRESS_CLIENT_ERR_BUSY);
    REQUIRE(!busy && !atomic_load(&release_open));
    fixture_readable(slow);
    char extra;
    REQUIRE(recv(slow, &extra, 1u, 0) == 0); close(slow);
    REQUIRE(!atomic_load(&release_open));
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &third, NULL) == MAELYS_EGRESS_CLIENT_OK);

    /* Lease violations are processed even while the first destruction waits.
     * Observe EOF on both leases, then all three slots are retiring, not free.
     * The client implementation is included above so these private descriptors
     * can be observed without a new public accessor. */
    REQUIRE(send(healthy->lease, "!", 1u, 0) == 1);
    fixture_readable(healthy->lease);
    REQUIRE(recv(healthy->lease, &extra, 1u, 0) == 0);
    REQUIRE(send(third->lease, "!", 1u, 0) == 1);
    fixture_readable(third->lease);
    REQUIRE(recv(third->lease, &extra, 1u, 0) == 0);
    maelys_egress_client_channel_close(healthy);
    maelys_egress_client_channel_close(third);
    for (unsigned i = 0u; i < 16u; ++i) {
        REQUIRE(maelys_egress_client_channel_open(path, 1000u, &busy, NULL) == MAELYS_EGRESS_CLIENT_ERR_BUSY);
        REQUIRE(!busy && !atomic_load(&destroyed));
    }

    /* A real HTTP proxy request completes WHILE the reaper is joining the
     * held channel. This would time out if channel destruction ran on the
     * proxy owner reactor. No release flag is set until the proof is complete. */
    int proxy = socket(AF_INET, SOCK_STREAM, 0); REQUIRE(proxy >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(maelys_egress_server_port(test.server))};
    REQUIRE(connect(proxy, (struct sockaddr *)&address, sizeof(address)) == 0);
    static const char denied[] = "CONNECT forbidden.invalid:443 HTTP/1.1\r\nHost: forbidden.invalid:443\r\nProxy-Authorization: Bearer 0123456789abcdef\r\n\r\n";
    REQUIRE(send(proxy, denied, sizeof(denied) - 1u, 0) == (ssize_t)(sizeof(denied) - 1u));
    fixture_readable(proxy); char response[512] = {0};
    REQUIRE(recv(proxy, response, sizeof(response) - 1u, 0) > 0);
    REQUIRE(strstr(response, "403") && !atomic_load(&release_open)); close(proxy);
    atomic_store(&release_open, 1);
    maelys_egress_client_channel_t *reused[3];
    for (size_t i = 0u; i < 3u; ++i) reused[i] = open_when_available(path);
    REQUIRE(atomic_load(&destroyed) == 3);
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &busy, NULL) == MAELYS_EGRESS_CLIENT_ERR_BUSY);
    int stream = -1;
    REQUIRE(maelys_egress_client_connect(maelys_egress_client_channel_fd(reused[0]),
        "forbidden.invalid", 443u, 1000u, &stream, NULL) == MAELYS_EGRESS_CLIENT_ERR_DENIED);
    REQUIRE(stream == -1);
    for (size_t i = 0u; i < 3u; ++i) maelys_egress_client_channel_close(reused[i]);
    REQUIRE(maelys_egress_channel_broker_destroy(broker, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(atomic_load(&destroyed) == 6);
    REQUIRE(access(path, F_OK) != 0);

    /* Server stop closes idle leases and removes the listener even while the
     * reaper is still held in an open. The retained connector keeps the
     * control object readable until both workers have been joined. */
    atomic_store(&entered, 0); atomic_store(&destroying, 0); atomic_store(&release_open, 0);
    REQUIRE(maelys_egress_channel_broker_create(connector, path, 1000u, 500u, 2u, &broker, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &client, NULL) == MAELYS_EGRESS_CLIENT_OK);
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &healthy, NULL) == MAELYS_EGRESS_CLIENT_OK);
    REQUIRE(maelys_sys_fd_send(maelys_egress_client_channel_fd(client), request, length, -1) == MAELYS_SYS_OK);
    wait_flag(&entered);
    maelys_egress_client_channel_close(client);
    wait_flag(&destroying);
    REQUIRE(maelys_egress_server_stop(test.server) == MAELYS_EGRESS_OK);
    REQUIRE(pthread_join(thread, NULL) == 0);
    for (unsigned i = 0; i < 1000u && access(path, F_OK) == 0; ++i) {
        struct timespec delay = {.tv_nsec = 1000000L}; nanosleep(&delay, NULL);
    }
    REQUIRE(access(path, F_OK) != 0 && !atomic_load(&release_open));
    fixture_readable(healthy->lease);
    REQUIRE(recv(healthy->lease, &extra, 1u, 0) == 0);
    REQUIRE(atomic_load(&destroyed) == 6);
    atomic_store(&release_open, 1);
    REQUIRE(maelys_egress_channel_broker_destroy(broker, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(atomic_load(&destroyed) == 8);
    maelys_egress_client_channel_close(healthy);
    maelys_egress_connector_release(connector);
    maelys_egress_config_destroy(test.config); maelys_egress_policy_destroy(test.policy);
    REQUIRE(rmdir(directory) == 0); free(directory);
    REQUIRE(fixture_fd_count() == baseline);
    puts("broker faults: partial delivery, bounded reaping, broker/proxy progress, deadline/lease isolation and server stop passed");
    return 0;
}
