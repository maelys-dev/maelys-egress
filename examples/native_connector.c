#include <maelys/egress.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

typedef struct server_thread_context {
    maelys_egress_server_t *server;
    maelys_egress_result_t result;
    char *error;
} server_thread_context_t;

static void *server_main(void *opaque) {
    server_thread_context_t *context = opaque;
    context->result = maelys_egress_server_run(context->server, &context->error);
    return NULL;
}

static int send_all(int fd, const void *data, size_t length) {
    const unsigned char *cursor = data;
    while (length) {
        ssize_t amount = send(fd, cursor, length, 0);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return 0;
        cursor += (size_t)amount;
        length -= (size_t)amount;
    }
    return 1;
}

int main(void) {
    static const char secret[] = "replace-this-example-secret";
    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    maelys_egress_server_t *server = NULL;
    maelys_egress_connector_t *connector = NULL;
    maelys_egress_session_t *session = NULL;
    char *error = NULL;
    int ok = maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK &&
        maelys_egress_policy_allow_tcp(policy, "example.com", 80u, 0, &error) ==
            MAELYS_EGRESS_OK &&
        maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK &&
        maelys_egress_config_create(&config, &error) == MAELYS_EGRESS_OK &&
        maelys_egress_config_set_authentication(
            config, "embedded-worker", secret, &error) == MAELYS_EGRESS_OK &&
        maelys_egress_config_set_native_only(config, 1, &error) ==
            MAELYS_EGRESS_OK &&
        maelys_egress_server_create(policy, config, &server, &error) ==
            MAELYS_EGRESS_OK &&
        maelys_egress_server_connector_create(
            server, "embedded-worker", secret, &connector, &error) ==
            MAELYS_EGRESS_OK;
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    if (!ok) {
        fprintf(stderr, "setup: %s\n", error ? error : "failed");
        maelys_egress_error_free(error);
        maelys_egress_connector_release(connector);
        maelys_egress_server_destroy(server);
        return 1;
    }
    server_thread_context_t thread_context = {.server = server};
    pthread_t thread;
    if (pthread_create(&thread, NULL, server_main, &thread_context) != 0) {
        fprintf(stderr, "cannot create Egress thread\n");
        maelys_egress_connector_release(connector);
        maelys_egress_server_destroy(server);
        return 1;
    }
    for (unsigned int attempt = 0u;
         attempt < 1000u && !maelys_egress_server_is_running(server);
         ++attempt) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&delay, NULL);
    }
    if (!maelys_egress_server_is_running(server)) {
        fprintf(stderr, "Egress did not enter its reactor\n");
        (void)maelys_egress_server_stop(server);
        (void)pthread_join(thread, NULL);
        maelys_egress_connector_release(connector);
        maelys_egress_server_destroy(server);
        return 1;
    }
    if (maelys_egress_connector_session_open(
            connector, "example.com", 80u, 5000u, &session, &error) !=
            MAELYS_EGRESS_OK) {
        fprintf(stderr, "session_open: %s\n", error ? error : "failed");
        maelys_egress_error_free(error);
        (void)maelys_egress_server_stop(server);
        (void)pthread_join(thread, NULL);
        maelys_egress_connector_release(connector);
        maelys_egress_server_destroy(server);
        return 1;
    }
    static const char request[] =
        "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    char response[1024];
    ssize_t received = send_all(maelys_egress_session_fd(session), request,
        sizeof(request) - 1u) ? recv(maelys_egress_session_fd(session),
            response, sizeof(response) - 1u, 0) : -1;
    if (received > 0) {
        response[received] = '\0';
        fputs(response, stdout);
    }
    maelys_egress_session_release(session);
    maelys_egress_connector_release(connector);
    (void)maelys_egress_server_stop(server);
    (void)pthread_join(thread, NULL);
    maelys_egress_error_free(thread_context.error);
    maelys_egress_server_destroy(server);
    return received > 0 ? 0 : 1;
}
