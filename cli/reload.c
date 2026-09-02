#include "cli/cli.h"

#include <signal.h>
#include <string.h>

/* SIGHUP policy reload: the signal waiter thread reloads the configuration
 * file, refuses any change outside the destinations, and swaps in a freshly
 * sealed policy generation. SIGINT and SIGTERM stop the server. */

void *egress_cli_signal_main(void *opaque) {
    signal_context_t *context = opaque;
    for (;;) {
        int signal_number = 0;
        if (sigwait(&context->signals, &signal_number) != 0) continue;
        if (signal_number != SIGHUP || !context->config_path) {
            (void)maelys_egress_server_stop(context->server);
            return NULL;
        }
        egress_cli_settings_t fresh;
        maelys_cli_error_t load_error;
        char *error = NULL;
        maelys_egress_policy_t *replacement = NULL;
        int loaded = egress_cli_settings_load(context->config_path, &fresh, &load_error) == 0;
        maelys_egress_result_t result = MAELYS_EGRESS_ERR_STATE;
        const char *rejection = NULL;
        if (!loaded) {
            rejection = load_error.message;
        } else if (!egress_cli_settings_control_equal(context->baseline, &fresh)) {
            rejection = "reload changes control-plane settings; only destinations may change";
        } else {
            result = egress_cli_build_policy(&fresh, &replacement, &error);
        }
        uint64_t generation = 0u;
        if (result == MAELYS_EGRESS_OK) {
            result = maelys_egress_server_replace_policy(
                context->server, replacement, &generation, &error);
        }
        if (result == MAELYS_EGRESS_OK) {
            egress_cli_lifecycle_policy_reloaded(generation,
                maelys_egress_policy_digest_hex(replacement));
        } else {
            egress_cli_lifecycle_message("policy-reload-rejected",
                rejection ? rejection : error ? error :
                maelys_egress_result_string(result));
        }
        maelys_egress_policy_destroy(replacement);
        maelys_egress_error_free(error);
        if (loaded) egress_cli_settings_destroy(&fresh);
    }
}
