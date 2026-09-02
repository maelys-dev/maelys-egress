#include <maelys/egress.h>

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct signal_context {
    maelys_egress_server_t *server;
    sigset_t signals;
} signal_context_t;

static int check_result(
    maelys_egress_result_t result,
    char **error,
    const char *operation) {
    if (result == MAELYS_EGRESS_OK) return 1;
    fprintf(stderr, "%s: %s\n", operation,
            *error ? *error : maelys_egress_result_string(result));
    maelys_egress_error_free(*error);
    *error = NULL;
    return 0;
}

static void receipt_sink(void *context, const maelys_egress_receipt_t *receipt) {
    (void)context;
    fprintf(stderr,
            "receipt id=%llu principal=%s destination=%s:%u result=%s "
            "generation=%llu bytes=%llu/%llu\n",
            (unsigned long long)maelys_egress_receipt_id(receipt),
            maelys_egress_receipt_principal(receipt),
            maelys_egress_receipt_host(receipt),
            (unsigned int)maelys_egress_receipt_port(receipt),
            maelys_egress_result_string(maelys_egress_receipt_result(receipt)),
            (unsigned long long)maelys_egress_receipt_policy_generation(receipt),
            (unsigned long long)maelys_egress_receipt_bytes_from_client(receipt),
            (unsigned long long)maelys_egress_receipt_bytes_to_client(receipt));
}

static void *signal_main(void *opaque) {
    signal_context_t *context = opaque;
    int signal_number = 0;
    if (sigwait(&context->signals, &signal_number) == 0) {
        (void)maelys_egress_server_stop(context->server);
    }
    return NULL;
}

int main(void) {
    static const char secret[] = "example-secret-change-before-production";
    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    maelys_egress_server_t *server = NULL;
    char *error = NULL;

    if (!check_result(maelys_egress_policy_create(&policy, &error), &error,
                      "policy_create") ||
        !check_result(maelys_egress_policy_allow_tcp(
                          policy, "github.com", 443u, 0, &error),
                      &error, "policy_allow_tcp") ||
        !check_result(maelys_egress_policy_require_tls_sni(
                          policy, "github.com", 443u, &error),
                      &error, "policy_require_tls_sni") ||
        !check_result(maelys_egress_policy_seal(policy, &error), &error,
                      "policy_seal") ||
        !check_result(maelys_egress_config_create(&config, &error), &error,
                      "config_create") ||
        !check_result(maelys_egress_config_set_listen(
                          config, "127.0.0.1", 0u, &error),
                      &error, "config_set_listen") ||
        !check_result(maelys_egress_config_set_authentication(
                          config, "example", secret, &error),
                      &error, "config_set_authentication")) {
        maelys_egress_config_destroy(config);
        maelys_egress_policy_destroy(policy);
        return 1;
    }

    maelys_egress_config_set_receipt_sink(config, receipt_sink, NULL);
    if (!check_result(maelys_egress_server_create(
                          policy, config, &server, &error),
                      &error, "server_create")) {
        maelys_egress_config_destroy(config);
        maelys_egress_policy_destroy(policy);
        return 1;
    }
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);

    signal_context_t signal_context = {.server = server};
    sigemptyset(&signal_context.signals);
    sigaddset(&signal_context.signals, SIGINT);
    sigaddset(&signal_context.signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signal_context.signals, NULL) != 0) {
        fprintf(stderr, "cannot block shutdown signals\n");
        maelys_egress_server_destroy(server);
        return 1;
    }
    pthread_t signal_thread;
    if (pthread_create(&signal_thread, NULL, signal_main, &signal_context) != 0) {
        fprintf(stderr, "cannot create signal waiter\n");
        maelys_egress_server_destroy(server);
        return 1;
    }

    printf("HTTP_PROXY=http://example:%s@127.0.0.1:%u\n", secret,
           (unsigned int)maelys_egress_server_port(server));
    printf("Press Ctrl-C to stop.\n");
    fflush(stdout);
    maelys_egress_result_t run_result = maelys_egress_server_run(server, &error);
    (void)pthread_kill(signal_thread, SIGTERM);
    (void)pthread_join(signal_thread, NULL);
    int ok = check_result(run_result, &error, "server_run");
    maelys_egress_server_destroy(server);
    return ok ? 0 : 1;
}
