/* SPDX-License-Identifier: MPL-2.0 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "src/internal.h"
#include "maelys/egress_client.h"
#include "maelys/sys/fdpass.h"
#include "tests/tls_socket_fixture.h"
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>

/* Compile the real state machines. Only OS boundaries are substituted; no
 * production hooks, replacement fd-passing implementation or public API.
 * Atomic counters expose completed destruction/join and connector ownership,
 * not racy reads of the reactor's private slots. */
#include "src/connector.c"
static atomic_int workers_started, joined, created, destroyed;
static maelys_sys_result_t gate_start(const char *name, maelys_sys_thread_fn fn,
    void *context, maelys_sys_thread_t **thread) {
    maelys_sys_result_t result = maelys_sys_thread_create(name, fn, context, thread);
    if (result == MAELYS_SYS_OK) atomic_fetch_add(&workers_started, 1);
    return result;
}
static maelys_sys_result_t gate_join(maelys_sys_thread_t **thread, void **result) {
    maelys_sys_result_t status = maelys_sys_thread_join(thread, result);
    if (status == MAELYS_SYS_OK) atomic_fetch_add(&joined, 1);
    return status;
}
#define maelys_sys_thread_create gate_start
#define maelys_sys_thread_join gate_join
#include "src/channel_server.c"
#undef maelys_sys_thread_join
#undef maelys_sys_thread_create

static maelys_egress_result_t gate_create(maelys_egress_connector_t *connector,
    uint64_t timeout, maelys_egress_channel_t **channel, int *fd, char **error) {
    maelys_egress_result_t result = maelys_egress_channel_create(connector, timeout, channel, fd, error);
    if (result == MAELYS_EGRESS_OK) atomic_fetch_add(&created, 1);
    return result;
}
static void gate_destroy(maelys_egress_channel_t *channel) {
    int present = channel != NULL;
    maelys_egress_channel_destroy(channel);
    if (present) atomic_fetch_add(&destroyed, 1);
}

enum { SEND_NORMAL, SEND_FAIL, SEND_PARTIAL_FAIL, SEND_PARTIAL_BLOCK };
static atomic_int send_mode, sent_bytes, sent_rights, send_attempts;
static atomic_int broker_short, broker_calls, broker_bytes, broker_truncate;
static atomic_int client_short, client_calls, client_bytes, client_truncate;
static atomic_int wrong_owner;

static maelys_sys_result_t gate_send(int fd, const void *bytes, size_t length,
    int passed, size_t *sent) {
    int mode = atomic_load(&send_mode);
    atomic_fetch_add(&send_attempts, 1);
    *sent = 0u;
    if (mode == SEND_FAIL || (mode == SEND_PARTIAL_FAIL && atomic_load(&sent_bytes)))
        return MAELYS_SYS_ERR_CLOSED;
    if (mode == SEND_PARTIAL_BLOCK && atomic_load(&sent_bytes))
        return MAELYS_SYS_ERR_WOULD_BLOCK;
    if (mode == SEND_PARTIAL_FAIL || mode == SEND_PARTIAL_BLOCK) length = 1u;
    maelys_sys_result_t result = maelys_sys_fd_stream_send(fd, bytes, length, passed, sent);
    if (result == MAELYS_SYS_OK) {
        atomic_fetch_add(&sent_bytes, (int)*sent);
        if (*sent && passed >= 0) atomic_fetch_add(&sent_rights, 1);
    }
    return result;
}

static maelys_sys_result_t gate_receive(int fd, void *bytes, size_t capacity,
    size_t *received, int *fds, size_t fd_capacity, size_t *count, unsigned *flags,
    atomic_int *short_reads, atomic_int *calls, atomic_int *total, atomic_int *truncate) {
    if (atomic_load(short_reads)) {
        int call = atomic_fetch_add(calls, 1);
        /* Force retries while readiness is asserted, then exactly one byte
         * per successful read even if the kernel coalesces every write. */
        if (call < 2) {
            *received = *count = 0u; *flags = 0u;
            if (!call) return MAELYS_SYS_ERR_WOULD_BLOCK;
            errno = EINTR; return MAELYS_SYS_ERR_OS;
        }
        capacity = 1u;
    }
    maelys_sys_result_t result = maelys_sys_fd_stream_receive(fd, bytes, capacity,
        received, fds, fd_capacity, count, flags);
    if (result == MAELYS_SYS_OK) {
        int previous = atomic_fetch_add(total, (int)*received);
        int at = atomic_load(truncate);
        if (at && previous < at && previous + (int)*received >= at) {
            *flags |= MAELYS_SYS_FDPASS_CONTROL_TRUNCATED;
            atomic_store(truncate, 0);
        }
    }
    return result;
}
static maelys_sys_result_t gate_broker_receive(int fd, void *bytes, size_t capacity,
    size_t *received, int *fds, size_t fd_capacity, size_t *count, unsigned *flags) {
    return gate_receive(fd, bytes, capacity, received, fds, fd_capacity, count, flags,
        &broker_short, &broker_calls, &broker_bytes, &broker_truncate);
}
static uid_t gate_uid(void) {
    return atomic_load(&wrong_owner) ? (geteuid() ^ (uid_t)1) : geteuid();
}
#define geteuid gate_uid
#define maelys_sys_fd_stream_send gate_send
#define maelys_sys_fd_stream_receive gate_broker_receive
#define maelys_egress_channel_create gate_create
#define maelys_egress_channel_destroy gate_destroy
#include "src/channel_broker.c"
#undef maelys_egress_channel_destroy
#undef maelys_egress_channel_create
#undef maelys_sys_fd_stream_receive
#undef maelys_sys_fd_stream_send
#undef geteuid

static maelys_sys_result_t gate_client_receive(int fd, void *bytes, size_t capacity,
    size_t *received, int *fds, size_t fd_capacity, size_t *count, unsigned *flags) {
    return gate_receive(fd, bytes, capacity, received, fds, fd_capacity, count, flags,
        &client_short, &client_calls, &client_bytes, &client_truncate);
}
#define maelys_sys_fd_stream_receive gate_client_receive
#include "client/channel_open.c"
#undef maelys_sys_fd_stream_receive

static void gate_pause(void) {
    struct timespec delay = {.tv_nsec = 1000000L}; nanosleep(&delay, NULL);
}
static void gate_wait(atomic_int *value, int minimum) {
    uint64_t deadline = now_ms() + 2000u;
    while (atomic_load(value) < minimum && now_ms() < deadline) gate_pause();
    REQUIRE(atomic_load(value) >= minimum);
}
static int gate_peer(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); REQUIRE(fd >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    REQUIRE(strlen(path) < sizeof(address.sun_path)); strcpy(address.sun_path, path);
    REQUIRE(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    fixture_nonblocking(fd);
    return fd;
}
static void gate_request(int fd) {
    unsigned char request[8]; egress_bootstrap_request(request);
    REQUIRE(send(fd, request, sizeof(request), 0) == (ssize_t)sizeof(request));
}
static void gate_eof(int fd) {
    fixture_readable(fd);
    unsigned char bytes[16]; size_t received = 0u, count = 0u; unsigned flags = 0u;
    maelys_sys_result_t result = maelys_sys_fd_stream_receive(fd, bytes, sizeof(bytes),
        &received, NULL, 0u, &count, &flags);
    REQUIRE(result == MAELYS_SYS_ERR_CLOSED || result == MAELYS_SYS_ERR_RESET);
}
static void gate_idle(maelys_egress_connector_t *connector, int fds) {
    uint64_t deadline = now_ms() + 2000u;
    while (now_ms() < deadline &&
        (atomic_load(&destroyed) != atomic_load(&created) ||
         atomic_load(&joined) != atomic_load(&workers_started) ||
         atomic_load(&connector->references) != 2u || fixture_fd_count() != fds)) gate_pause();
    REQUIRE(atomic_load(&created) == atomic_load(&destroyed));
    REQUIRE(atomic_load(&workers_started) == atomic_load(&joined));
    REQUIRE(atomic_load(&connector->references) == 2u); /* caller + broker */
    REQUIRE(fixture_fd_count() == fds);
}
static maelys_egress_client_channel_t *gate_open(const char *path) {
    uint64_t deadline = now_ms() + 2000u;
    maelys_egress_client_channel_t *client = NULL;
    maelys_egress_client_result_t result;
    do {
        result = maelys_egress_client_channel_open(path, 1000u, &client, NULL);
        if (result != MAELYS_EGRESS_CLIENT_ERR_BUSY) break;
        gate_pause();
    } while (now_ms() < deadline);
    REQUIRE(result == MAELYS_EGRESS_CLIENT_OK && client);
    return client;
}
static void gate_reset_io(void) {
    atomic_store(&send_mode, SEND_NORMAL); atomic_store(&sent_bytes, 0);
    atomic_store(&sent_rights, 0); atomic_store(&send_attempts, 0);
    atomic_store(&broker_short, 0); atomic_store(&broker_calls, 0);
    atomic_store(&broker_bytes, 0); atomic_store(&broker_truncate, 0);
    atomic_store(&client_short, 0); atomic_store(&client_calls, 0);
    atomic_store(&client_bytes, 0); atomic_store(&client_truncate, 0);
}

static void gate_fragments(const char *path, maelys_egress_connector_t *connector, int fds) {
    puts("bootstrap gates: forced short reads and CONTROL_TRUNCATED");
    /* Both complete frames are written normally; reads cannot coalesce. */
    gate_reset_io(); atomic_store(&broker_short, 1); atomic_store(&client_short, 1);
    maelys_egress_client_channel_t *client = gate_open(path);
    REQUIRE(atomic_load(&broker_calls) == 10 && atomic_load(&broker_bytes) == 8);
    REQUIRE(atomic_load(&client_calls) == 19 && atomic_load(&client_bytes) == 16);
    REQUIRE(atomic_load(&sent_rights) == 1);
    maelys_egress_client_channel_close(client); gate_idle(connector, fds);
    /* Truncation at the first descriptor and after it has been retained.
     * System still performs the real receive and ownership transfer. */
    for (int at = 1; at <= 2; ++at) {
        gate_reset_io(); atomic_store(&client_short, 1); atomic_store(&client_truncate, at);
        client = NULL;
        REQUIRE(maelys_egress_client_channel_open(path, 1000u, &client, NULL) == MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
        REQUIRE(!client && !atomic_load(&client_truncate)); gate_idle(connector, fds);
    }
    gate_reset_io(); atomic_store(&broker_truncate, 1);
    client = NULL;
    REQUIRE(maelys_egress_client_channel_open(path, 1000u, &client, NULL) == MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    REQUIRE(!client && !atomic_load(&broker_truncate)); gate_idle(connector, fds);
    gate_reset_io(); client = gate_open(path);
    atomic_store(&broker_truncate, 9); /* eight request bytes already read */
    REQUIRE(send(client->lease, "!", 1u, 0) == 1); gate_eof(client->lease);
    REQUIRE(!atomic_load(&broker_truncate));
    maelys_egress_client_channel_close(client); gate_idle(connector, fds);
    gate_reset_io();
}

static void gate_delivery(const char *path, maelys_egress_connector_t *connector, int fds) {
    for (int mode = SEND_FAIL; mode <= SEND_PARTIAL_BLOCK; ++mode) {
        printf("bootstrap gates: delivery failure mode %d\n", mode);
        gate_reset_io(); atomic_store(&send_mode, mode);
        int before = atomic_load(&created);
        int peer = gate_peer(path); gate_request(peer);
        gate_wait(&created, before + 1); gate_wait(&send_attempts, 1);
        int passed = -1;
        if (mode != SEND_FAIL) {
            fixture_readable(peer);
            unsigned char byte; size_t received = 0u, count = 0u; unsigned flags = 0u;
            REQUIRE(maelys_sys_fd_stream_receive(peer, &byte, 1u, &received,
                &passed, 1u, &count, &flags) == MAELYS_SYS_OK);
            REQUIRE(byte == 'M' && received == 1u && count == 1u && !flags && passed >= 0);
        }
        /* Includes permanently unwritable after the first descriptor byte:
         * the broker's absolute handshake deadline must reclaim the channel. */
        gate_eof(peer); close(peer); if (passed >= 0) close(passed);
        REQUIRE(atomic_load(&created) == before + 1);
        REQUIRE(atomic_load(&sent_rights) == (mode == SEND_FAIL ? 0 : 1));
        gate_idle(connector, fds);
        gate_reset_io();
        maelys_egress_client_channel_t *next = gate_open(path); /* sole slot recovered */
        maelys_egress_client_channel_close(next); gate_idle(connector, fds);
    }
}

static void gate_trickle(const char *path, maelys_egress_connector_t *connector, int fds) {
    puts("bootstrap gates: absolute trickle deadline");
    gate_reset_io();
    int peer = gate_peer(path);
    unsigned char request[8]; egress_bootstrap_request(request);
    uint64_t start = now_ms();
    /* A 300 ms deadline, but a new fragment at 0, 100 and 200 ms. Wait for
     * actual consumption of each fragment: writes alone prove nothing.
     * At 400 ms the correct peer is closed, the sliding-deadline mutant is
     * still waiting until >=500 ms. Scheduler delays fail, never pass falsely. */
    for (int i = 0; i < 3; ++i) {
        while (now_ms() < start + (uint64_t)i * 100u) gate_pause();
        REQUIRE(send(peer, request + i, 1u, 0) == 1);
        gate_wait(&broker_bytes, i + 1);
        REQUIRE(now_ms() - start < 280u);
    }
    /* Another handshake must complete while the trickler is still alive. */
    maelys_egress_client_channel_t *healthy = gate_open(path);
    REQUIRE(now_ms() - start < 280u);
    maelys_egress_client_channel_close(healthy);
    struct pollfd item = {.fd = peer, .events = POLLIN};
    int remaining = (int)(start + 420u - now_ms());
    REQUIRE(remaining > 0 && poll(&item, 1u, remaining) == 1);
    REQUIRE(now_ms() - start < 450u); gate_eof(peer); close(peer);
    REQUIRE(atomic_load(&broker_bytes) == 11); gate_idle(connector, fds);
}

typedef struct gate_flood { const char *path; atomic_int stop, completed; } gate_flood_t;
static void *gate_flood_main(void *opaque) {
    gate_flood_t *flood = opaque;
    while (!atomic_load(&flood->stop)) {
        int peer = gate_peer(flood->path);
        unsigned char request[8]; egress_bootstrap_request(request);
#ifdef __APPLE__
        fixture_rights(peer, request, 0u, 50u); /* genuinely control-only */
#else
        fixture_rights(peer, request, sizeof(request), 50u);
#endif
        unsigned char response[16]; size_t progress = 0u;
        while (progress < sizeof(response)) {
            fixture_readable(peer);
            size_t received = 0u, count = 0u; unsigned flags = 0u;
            REQUIRE(maelys_sys_fd_stream_receive(peer, response + progress, sizeof(response) - progress,
                &received, NULL, 0u, &count, &flags) == MAELYS_SYS_OK);
            REQUIRE(received && !count && !flags); progress += received;
        }
        unsigned status; uint64_t timeout;
        REQUIRE(egress_bootstrap_decode_response(response, sizeof(response), &status, &timeout));
        REQUIRE(status == EGRESS_BOOTSTRAP_MALFORMED);
        close(peer); atomic_fetch_add(&flood->completed, 1);
    }
    return NULL;
}
static void gate_fairness(const char *path, maelys_egress_connector_t *connector, int fds) {
    puts("bootstrap gates: control flood and non-reading peer fairness");
    gate_reset_io();
    int unread = gate_peer(path); gate_request(unread); gate_wait(&sent_bytes, 16);
    /* This peer never receives the descriptor: delivery to the kernel is
     * already complete. It consumes one bounded lease, not the whole broker. */
    gate_flood_t flood = {.path = path}; pthread_t worker;
    REQUIRE(pthread_create(&worker, NULL, gate_flood_main, &flood) == 0);
    gate_wait(&flood.completed, 5);
    int slow = gate_peer(path); REQUIRE(send(slow, "M", 1u, 0) == 1);
    uint64_t start = now_ms(); int prior = atomic_load(&flood.completed);
    maelys_egress_client_channel_t *healthy = gate_open(path);
    REQUIRE(now_ms() - start < 250u);
    maelys_egress_client_channel_close(healthy);
    gate_eof(slow); REQUIRE(now_ms() - start < 450u); close(slow);
    REQUIRE(atomic_load(&flood.completed) > prior);
    atomic_store(&flood.stop, 1); REQUIRE(pthread_join(worker, NULL) == 0);
    close(unread); gate_idle(connector, fds);
}

typedef struct gate_server {
    maelys_egress_policy_t *policy; maelys_egress_config_t *config;
    maelys_egress_server_t *server; atomic_int ready;
} gate_server_t;
static void *gate_run_server(void *opaque) {
    gate_server_t *test = opaque;
    REQUIRE(maelys_egress_server_create(test->policy, test->config, &test->server, NULL) == MAELYS_EGRESS_OK);
    atomic_store(&test->ready, 1);
    REQUIRE(maelys_egress_server_run(test->server, NULL) == MAELYS_EGRESS_OK);
    maelys_egress_server_destroy(test->server); return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fixture_limits(); signal(SIGPIPE, SIG_IGN);
    int baseline = fixture_fd_count();
    /* Fork BEFORE starting any server/broker thread or opening its sockets.
     * The child inherits neither server resources nor multithreaded locks;
     * its activation pipe is opened only when the death test is ready. */
    int activate[2], ready_pipe[2];
    REQUIRE(pipe(activate) == 0 && pipe(ready_pipe) == 0);
    pid_t child = fork(); REQUIRE(child >= 0);
    if (!child) {
        alarm(60); close(activate[1]); close(ready_pipe[0]);
        char child_path[104]; size_t progress = 0u;
        while (progress < sizeof(child_path)) {
            ssize_t amount = read(activate[0], child_path + progress, sizeof(child_path) - progress);
            if (amount == 0) _exit(0); /* parent assertion failed before activation */
            REQUIRE(amount > 0); progress += (size_t)amount;
        }
        close(activate[0]);
        maelys_egress_client_channel_t *client = gate_open(child_path);
        REQUIRE(write(ready_pipe[1], "R", 1u) == 1);
        for (;;) pause(); /* parent SIGKILLs: no userspace cleanup */
        maelys_egress_client_channel_close(client);
    }
    close(activate[0]); close(ready_pipe[1]);
    gate_server_t test = {0};
    REQUIRE(maelys_egress_policy_create(&test.policy, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(maelys_egress_policy_allow_tcp(test.policy, "127.0.0.1", 9u, 1, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(maelys_egress_policy_seal(test.policy, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(maelys_egress_config_create(&test.config, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(maelys_egress_config_set_authentication(test.config, "native", "0123456789abcdef", NULL) == MAELYS_EGRESS_OK);
    pthread_t server; REQUIRE(pthread_create(&server, NULL, gate_run_server, &test) == 0);
    gate_wait(&test.ready, 1);
    while (!maelys_egress_server_is_running(test.server)) gate_pause();
    maelys_egress_connector_t *connector = NULL;
    REQUIRE(maelys_egress_server_connector_create(test.server, "native", "0123456789abcdef", &connector, NULL) == MAELYS_EGRESS_OK);
    char template[] = "/tmp/egress-bootstrap-gates-XXXXXX";
    REQUIRE(mkdtemp(template)); char *directory = realpath(template, NULL); REQUIRE(directory);
    char path[104] = {0}; REQUIRE(snprintf(path, sizeof(path), "%s/s", directory) < (int)sizeof(path));
    maelys_egress_channel_broker_t *broker = NULL;
    int before = fixture_fd_count();
    atomic_store(&wrong_owner, 1);
    REQUIRE(maelys_egress_channel_broker_create(connector, path, 1000u, 300u, 1u, &broker, NULL) != MAELYS_EGRESS_OK);
    REQUIRE(!broker && access(path, F_OK) != 0 && fixture_fd_count() == before);
    atomic_store(&wrong_owner, 0);
    char link[104], through[104], sub[104];
    REQUIRE(snprintf(link, sizeof(link), "%s/link", directory) < (int)sizeof(link));
    REQUIRE(symlink(directory, link) == 0);
    REQUIRE(snprintf(sub, sizeof(sub), "%s/sub", directory) < (int)sizeof(sub));
    REQUIRE(mkdir(sub, 0700) == 0);
    for (int ancestor = 0; ancestor < 2; ++ancestor) {
        REQUIRE(snprintf(through, sizeof(through), "%s/link/%ss", directory, ancestor ? "sub/" : "") < (int)sizeof(through));
        REQUIRE(maelys_egress_channel_broker_create(connector, through, 1000u, 300u, 1u, &broker, NULL) != MAELYS_EGRESS_OK);
        REQUIRE(!broker && access(through, F_OK) != 0 && fixture_fd_count() == before);
    }
    REQUIRE(unlink(link) == 0 && rmdir(sub) == 0);
    REQUIRE(atomic_load(&connector->references) == 1u);
    REQUIRE(maelys_egress_channel_broker_create(connector, path, 1000u, 300u, 1u, &broker, NULL) == MAELYS_EGRESS_OK);
    int fds = fixture_fd_count();
    gate_fragments(path, connector, fds);
    gate_delivery(path, connector, fds);

    /* Real process death. Readiness proves a live lease before SIGKILL. */
    puts("bootstrap gates: process death on idle lease");
    REQUIRE(write(activate[1], path, sizeof(path)) == (ssize_t)sizeof(path));
    fixture_readable(ready_pipe[0]);
    char ready; REQUIRE(read(ready_pipe[0], &ready, 1u) == 1 && ready == 'R');
    REQUIRE(atomic_load(&connector->references) == 3u);
    REQUIRE(kill(child, SIGKILL) == 0); int status;
    REQUIRE(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    gate_idle(connector, fds);
    maelys_egress_client_channel_t *next = gate_open(path);
    maelys_egress_client_channel_close(next); gate_idle(connector, fds);
    REQUIRE(maelys_egress_channel_broker_destroy(broker, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(maelys_egress_channel_broker_create(connector, path, 1000u, 300u, 4u, &broker, NULL) == MAELYS_EGRESS_OK);
    gate_trickle(path, connector, fixture_fd_count());
    gate_fairness(path, connector, fixture_fd_count());
    REQUIRE(maelys_egress_channel_broker_destroy(broker, NULL) == MAELYS_EGRESS_OK);
    REQUIRE(atomic_load(&connector->references) == 1u);
    maelys_egress_connector_release(connector);
    REQUIRE(maelys_egress_server_stop(test.server) == MAELYS_EGRESS_OK);
    REQUIRE(pthread_join(server, NULL) == 0);
    maelys_egress_config_destroy(test.config); maelys_egress_policy_destroy(test.policy);
    REQUIRE(rmdir(directory) == 0); free(directory);
    close(activate[1]); close(ready_pipe[0]);
    REQUIRE(fixture_fd_count() == baseline);
    puts("bootstrap gates: deterministic reads/truncation, delivery cleanup, absolute deadline, flood fairness, process death and path ownership passed");
    return 0;
}
