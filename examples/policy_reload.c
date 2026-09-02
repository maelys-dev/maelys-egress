#include <maelys/egress.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef struct control_context {
    maelys_egress_server_t *server;
    const maelys_egress_policy_t *replacement;
    maelys_egress_result_t result;
} control_context_t;

static maelys_egress_policy_t *private_policy(uint16_t port) {
    maelys_egress_policy_t *policy = NULL;
    char *error = NULL;
    maelys_egress_result_t result = maelys_egress_policy_create(&policy, &error);
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_policy_allow_tcp(
        policy, "127.0.0.1", port, 1, &error);
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_policy_seal(policy, &error);
    if (result != MAELYS_EGRESS_OK) {
        fprintf(stderr, "build policy: %s\n",
                error ? error : maelys_egress_result_string(result));
        maelys_egress_policy_destroy(policy);
        policy = NULL;
    }
    maelys_egress_error_free(error);
    return policy;
}

static void *control_main(void *opaque) {
    control_context_t *context = opaque;
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000L};
    (void)nanosleep(&delay, NULL);
    char *error = NULL;
    uint64_t generation = 0u;
    context->result = maelys_egress_server_replace_policy(
        context->server, context->replacement, &generation, &error);
    if (context->result == MAELYS_EGRESS_OK) {
        printf("installed policy generation %llu\n",
               (unsigned long long)generation);
    } else {
        fprintf(stderr, "replace policy: %s\n",
                error ? error : maelys_egress_result_string(context->result));
    }
    maelys_egress_error_free(error);
    (void)maelys_egress_server_stop(context->server);
    return NULL;
}

int main(void) {
    maelys_egress_policy_t *initial = private_policy(9u);
    maelys_egress_policy_t *replacement = private_policy(8u);
    maelys_egress_config_t *config = NULL;
    maelys_egress_server_t *server = NULL;
    char *error = NULL;
    if (!initial || !replacement ||
        maelys_egress_config_create(&config, &error) != MAELYS_EGRESS_OK ||
        maelys_egress_config_set_listen(config, "127.0.0.1", 0u, &error) !=
            MAELYS_EGRESS_OK ||
        maelys_egress_config_allow_unauthenticated_loopback(config, 1, &error) !=
            MAELYS_EGRESS_OK ||
        maelys_egress_server_create(initial, config, &server, &error) !=
            MAELYS_EGRESS_OK) {
        fprintf(stderr, "setup: %s\n", error ? error : "failed");
        maelys_egress_error_free(error);
        maelys_egress_server_destroy(server);
        maelys_egress_config_destroy(config);
        maelys_egress_policy_destroy(replacement);
        maelys_egress_policy_destroy(initial);
        return 1;
    }
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(initial);

    control_context_t context = {
        .server = server, .replacement = replacement, .result = MAELYS_EGRESS_OK};
    pthread_t control_thread;
    if (pthread_create(&control_thread, NULL, control_main, &context) != 0) {
        maelys_egress_server_destroy(server);
        maelys_egress_policy_destroy(replacement);
        return 1;
    }
    maelys_egress_result_t result = maelys_egress_server_run(server, &error);
    (void)pthread_join(control_thread, NULL);
    if (result != MAELYS_EGRESS_OK) {
        fprintf(stderr, "server_run: %s\n",
                error ? error : maelys_egress_result_string(result));
    }
    maelys_egress_error_free(error);
    maelys_egress_server_destroy(server);
    maelys_egress_policy_destroy(replacement);
    return result == MAELYS_EGRESS_OK && context.result == MAELYS_EGRESS_OK ? 0 : 1;
}
