/*
 * The supervising side of the mediated-connection channel: an Egress server
 * that opens no port (native_only), a connector bound to one principal, a
 * channel on that connector, and a child process that receives the channel's
 * client end on a descriptor number the supervisor chose and names in
 * MAELYS_EGRESS_CHANNEL_FD. The child links the client archive alone; this
 * program is the one that links the library. `maelys-egress channel exec` is
 * this supervisor as a command, for a program that needs no embedder.
 *
 *   example-channel_supervisor /absolute/path/to/example-channel_client [HOST PORT]
 *
 * Without HOST and PORT it serves a loopback echo itself, so the round trip
 * runs with no network at all: the child asks for 127.0.0.1:PORT, Egress
 * connects there, the echo answers, and the receipt says what happened.
 */
#include <maelys/egress.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHILD_CHANNEL_FD 3

extern char **environ;

/*
 * maelys_egress_server_run owns the thread that created the server, so the
 * server is created on the thread that runs it, and the main thread waits
 * for the word that it is up or that it could not be.
 */
typedef struct server_thread_context {
    maelys_egress_policy_t *policy;
    maelys_egress_config_t *config;
    maelys_egress_server_t *server;
    maelys_egress_result_t result;
    char *error;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    int announced;
} server_thread_context_t;

static void *server_main(void *opaque) {
    server_thread_context_t *context = opaque;
    context->result = maelys_egress_server_create(
        context->policy, context->config, &context->server, &context->error);
    (void)pthread_mutex_lock(&context->lock);
    context->announced = 1;
    (void)pthread_cond_signal(&context->condition);
    (void)pthread_mutex_unlock(&context->lock);
    if (context->result == MAELYS_EGRESS_OK) {
        context->result = maelys_egress_server_run(context->server, &context->error);
    }
    /* destroy belongs to the thread that created the server, like run. */
    maelys_egress_server_destroy(context->server);
    return NULL;
}

/* One connection, echoed back, then done: enough for the round trip. */
static void *echo_main(void *opaque) {
    int listener = (int)(intptr_t)opaque;
    int client;
    do { client = accept(listener, NULL, NULL); } while (client < 0 && errno == EINTR);
    (void)close(listener);
    if (client < 0) return NULL;
    char bytes[64];
    for (;;) {
        ssize_t amount = recv(client, bytes, sizeof(bytes), 0);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0 || send(client, bytes, (size_t)amount, 0) != amount) break;
    }
    (void)close(client);
    return NULL;
}

static int echo_listener(uint16_t *out_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET};
    socklen_t length = sizeof(address);
    if (fd < 0 || inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 1) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &length) != 0) {
        if (fd >= 0) (void)close(fd);
        return -1;
    }
    *out_port = ntohs(address.sin_port);
    return fd;
}

static void print_receipt(void *opaque, const maelys_egress_receipt_t *receipt) {
    (void)opaque;
    printf("supervisor: receipt %s:%u result %s, %llu bytes from the child\n",
           maelys_egress_receipt_host(receipt), (unsigned int)maelys_egress_receipt_port(receipt),
           maelys_egress_result_string(maelys_egress_receipt_result(receipt)),
           (unsigned long long)maelys_egress_receipt_bytes_from_client(receipt));
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 4) {
        fprintf(stderr, "usage: %s /absolute/path/to/example-channel_client [HOST PORT]\n", argv[0]);
        return 2;
    }
    const char *child_path = argv[1];
    if (child_path[0] != '/') {
        fprintf(stderr, "the child is started by absolute path, never through PATH\n");
        return 2;
    }
    const char *host = argc == 4 ? argv[2] : "127.0.0.1";
    uint16_t port = argc == 4 ? (uint16_t)strtoul(argv[3], NULL, 10) : 0u;
    pthread_t echo_thread;
    int echo_started = 0;
    if (argc == 2) {
        int listener = echo_listener(&port);
        if (listener < 0 || pthread_create(&echo_thread, NULL, echo_main,
                                           (void *)(intptr_t)listener) != 0) {
            fprintf(stderr, "cannot serve the loopback echo\n");
            return 1;
        }
        echo_started = 1;
    }

    static const char secret[] = "replace-this-example-secret";
    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    maelys_egress_connector_t *connector = NULL;
    maelys_egress_channel_t *channel = NULL;
    char *error = NULL;
    const char *step = NULL;
#define STEP(name, expression) (step = (name), (expression))
    int ok = STEP("policy", maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK) &&
        STEP("allow", maelys_egress_policy_allow_tcp(policy, host, port, argc == 2, &error) ==
             MAELYS_EGRESS_OK) &&
        STEP("seal", maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK) &&
        STEP("config", maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK) &&
        STEP("authentication", maelys_egress_config_set_authentication(
                 config, "confined", secret, &error) == MAELYS_EGRESS_OK) &&
        STEP("native_only", maelys_egress_config_set_native_only(config, 1, &error) ==
             MAELYS_EGRESS_OK);
    if (ok) maelys_egress_config_set_receipt_sink(config, print_receipt, NULL);
    server_thread_context_t thread_context = {
        .policy = policy, .config = config,
        .lock = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER
    };
    pthread_t thread;
    int thread_started = ok && STEP("thread", pthread_create(&thread, NULL, server_main,
                                                              &thread_context) == 0);
    if (thread_started) {
        (void)pthread_mutex_lock(&thread_context.lock);
        while (!thread_context.announced) {
            (void)pthread_cond_wait(&thread_context.condition, &thread_context.lock);
        }
        (void)pthread_mutex_unlock(&thread_context.lock);
    }
    maelys_egress_server_t *server = thread_context.server;
    for (unsigned int attempt = 0u; server && attempt < 1000u &&
         !maelys_egress_server_is_running(server); ++attempt) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&delay, NULL);
    }
    int client_fd = -1;
    ok = thread_started && STEP("server", server != NULL) &&
        STEP("running", maelys_egress_server_is_running(server)) &&
        STEP("connector", maelys_egress_server_connector_create(
                 server, "confined", secret, &connector, &error) == MAELYS_EGRESS_OK) &&
        STEP("channel", maelys_egress_channel_create(connector, 5000u, &channel, &client_fd,
                                                     &error) == MAELYS_EGRESS_OK);
#undef STEP
    int status = 1;
    if (!ok) {
        fprintf(stderr, "setup: %s: %s\n", step ? step : "?",
                error ? error : thread_context.error ? thread_context.error : "failed");
    } else {
        /* The child gets the channel's client end on CHILD_CHANNEL_FD and
         * nothing else of ours: every other descriptor is CLOEXEC. It learns
         * the number and the connect bound from its environment, the same
         * two variables `maelys-egress channel exec` sets. */
        char port_text[8];
        (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned int)port);
        char fd_text[8];
        (void)snprintf(fd_text, sizeof(fd_text), "%d", CHILD_CHANNEL_FD);
        (void)setenv("MAELYS_EGRESS_CHANNEL_FD", fd_text, 1);
        (void)setenv("MAELYS_EGRESS_CHANNEL_CONNECT_TIMEOUT_MS", "5000", 1);
        char *child_argv[] = {(char *)child_path, (char *)host, port_text, NULL};
        pid_t child = fork();
        if (child == 0) {
            if (dup2(client_fd, CHILD_CHANNEL_FD) != CHILD_CHANNEL_FD) _exit(127);
            execve(child_path, child_argv, environ);
            _exit(127);
        }
        (void)close(client_fd);
        int wait_status = 0;
        if (child > 0 && waitpid(child, &wait_status, 0) == child && WIFEXITED(wait_status)) {
            status = WEXITSTATUS(wait_status);
        }
        printf("supervisor: the child exited with %d\n", status);
    }
    maelys_egress_channel_destroy(channel);
    maelys_egress_connector_release(connector);
    if (thread_started) {
        if (server) (void)maelys_egress_server_stop(server);
        (void)pthread_join(thread, NULL);
    }
    server = NULL;
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    if (echo_started) {
        /* If no child ever connected, accept is still waiting: connect once
         * to release it, then join. */
        int wake = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
        if (wake >= 0 && inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1) {
            (void)connect(wake, (struct sockaddr *)&address, sizeof(address));
        }
        (void)pthread_join(echo_thread, NULL);
        if (wake >= 0) (void)close(wake);
    }
    maelys_egress_error_free(thread_context.error);
    maelys_egress_error_free(error);
    return status;
}
