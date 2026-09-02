#include <maelys/egress.h>

#include <stdint.h>
#include <stdio.h>

int main(void) {
    maelys_egress_policy_t *policy = NULL;
    maelys_egress_config_t *config = NULL;
    maelys_egress_server_t *server = NULL;
    maelys_egress_metrics_t *metrics = NULL;
    char *error = NULL;
    maelys_egress_result_t result = maelys_egress_policy_create(&policy, &error);
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_policy_allow_tcp(
        policy, "127.0.0.1", 9u, 1, &error);
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_policy_seal(policy, &error);
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_config_create(&config, &error);
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_config_set_listen(
        config, "127.0.0.1", 0u, &error);
    if (result == MAELYS_EGRESS_OK) result =
        maelys_egress_config_allow_unauthenticated_loopback(config, 1, &error);
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_server_create(
        policy, config, &server, &error);
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_server_metrics_snapshot(
        server, &metrics, &error);
    if (result == MAELYS_EGRESS_OK) {
        printf("generation=%llu accepted=%llu active=%llu admitted=%llu denied=%llu\n",
               (unsigned long long)maelys_egress_metrics_policy_generation(metrics),
               (unsigned long long)maelys_egress_metrics_accepted(metrics),
               (unsigned long long)maelys_egress_metrics_active(metrics),
               (unsigned long long)maelys_egress_metrics_admitted(metrics),
               (unsigned long long)maelys_egress_metrics_denied(metrics));
    } else {
        fprintf(stderr, "%s\n", error ? error : maelys_egress_result_string(result));
    }
    maelys_egress_error_free(error);
    maelys_egress_metrics_destroy(metrics);
    maelys_egress_server_destroy(server);
    maelys_egress_config_destroy(config);
    maelys_egress_policy_destroy(policy);
    return result == MAELYS_EGRESS_OK ? 0 : 1;
}
