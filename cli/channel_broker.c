/* SPDX-License-Identifier: MPL-2.0 */
#include "cli/cli.h"

#include <stdatomic.h>
#include <time.h>
#include "maelys/sys/clock.h"

typedef struct broker_command {
    signal_context_t signals;
    const char *digest;
    egress_cli_output_gate_t *gate;
    atomic_int server_finished;
    int ready;
    maelys_egress_result_t result;
    char *error;
} broker_command_t;

static void pause_briefly(void) {
    struct timespec delay = {.tv_nsec = 10000000L};
    (void)nanosleep(&delay, NULL);
}

/* The main thread creates/runs/destroys the server's owner-thread reactor.
 * This coordinator starts and monitors the broker, and joins its cleanup
 * worker only outside that reactor. Signals stay blocked in all workers;
 * the ordinary signal waiter is started only after ready is published. */
static void *broker_command_main(void *opaque) {
    broker_command_t *command = opaque;
    const egress_cli_settings_t *settings = command->signals.baseline;
    maelys_egress_server_t *server = command->signals.server;
    maelys_egress_connector_t *connector = NULL;
    maelys_egress_channel_broker_t *broker = NULL;
    pthread_t signal_thread;
    int signal_started = 0;
    uint64_t deadline = 0u, now = 0u;
    command->result = MAELYS_EGRESS_ERR_STATE;
    if (maelys_sys_deadline_after(5000u, &deadline) != MAELYS_SYS_OK) goto done;
    while (!maelys_egress_server_is_running(server)) {
        if (atomic_load(&command->server_finished) ||
            maelys_sys_monotonic_ms(&now) != MAELYS_SYS_OK || now >= deadline) goto done;
        pause_briefly();
    }
    command->result = maelys_egress_server_native_connector_create(server, &connector, &command->error);
    if (command->result != MAELYS_EGRESS_OK) goto done;
    command->result = maelys_egress_channel_broker_create(connector, settings->channel_listen_unix,
        settings->channel_connect_timeout_ms, settings->channel_handshake_timeout_ms,
        settings->channel_max_clients, &broker, &command->error);
    if (command->result != MAELYS_EGRESS_OK) goto done;
    egress_cli_lifecycle_ready(NULL, NULL, 0u, settings->admin_host,
        maelys_egress_server_admin_port(server), command->digest, settings->channel_listen_unix);
    command->ready = 1;
    egress_cli_output_gate_open(command->gate, 1);
    if (pthread_create(&signal_thread, NULL, egress_cli_signal_main, &command->signals) != 0) {
        command->result = MAELYS_EGRESS_ERR_MEMORY;
        goto done;
    }
    signal_started = 1;
    while (!atomic_load(&command->server_finished) && maelys_egress_server_is_running(server)) {
        if (!maelys_egress_channel_broker_is_running(broker)) {
            /* SIGTERM can stop the server and broker between these snapshots.
             * A broker stopped by the server is not an independent failure. */
            if (!atomic_load(&command->server_finished) && maelys_egress_server_is_running(server))
                command->result = MAELYS_EGRESS_ERR_IO;
            break;
        }
        pause_briefly();
    }
done:
    if (!command->ready) egress_cli_output_gate_open(command->gate, -1);
    (void)maelys_egress_server_stop(server);
    if (signal_started) {
        (void)pthread_kill(signal_thread, SIGTERM);
        (void)pthread_join(signal_thread, NULL);
    }
    char *cleanup_error = NULL;
    maelys_egress_result_t cleanup = maelys_egress_channel_broker_destroy(broker, &cleanup_error);
    if (command->result == MAELYS_EGRESS_OK && cleanup != MAELYS_EGRESS_OK) {
        command->result = cleanup;
    }
    if (!command->error && cleanup_error) {
        command->error = cleanup_error;
    } else maelys_egress_error_free(cleanup_error);
    maelys_egress_connector_release(connector);
    return NULL;
}

int egress_cli_channel_run(maelys_egress_server_t *server,
    const egress_cli_settings_t *settings, const char *path, const sigset_t *signals,
    const char *digest, egress_cli_output_gate_t *gate, maelys_cli_error_t *error) {
    broker_command_t command = {
        .signals = {.server = server, .signals = *signals, .config_path = path, .baseline = settings},
        .digest = digest, .gate = gate
    };
    atomic_init(&command.server_finished, 0);
    pthread_t worker;
    if (pthread_create(&worker, NULL, broker_command_main, &command) != 0) {
        egress_cli_output_gate_open(gate, -1);
        maelys_cli_error_set(error, MAELYS_CLI_CODE_UNEXPECTED,
            "Check available process/thread resources.", "cannot create broker coordinator");
        return -1;
    }
    char *server_error = NULL;
    maelys_egress_result_t result = maelys_egress_server_run(server, &server_error);
    atomic_store(&command.server_finished, 1);
    (void)pthread_join(worker, NULL);
    if (command.result != MAELYS_EGRESS_OK) result = command.result;
    const char *message = command.error ? command.error : server_error ? server_error :
        maelys_egress_result_string(result);
    int status = result == MAELYS_EGRESS_OK ? 0 : 1;
    if (!command.ready) {
        maelys_cli_error_set(error, MAELYS_CLI_CODE_IO_FAILED,
            "Check channel socket parent ownership/mode, path availability and process resources.",
            "%s", message);
        status = -1;
    } else if (status) egress_cli_lifecycle_message("fatal", message);
    else egress_cli_lifecycle_message("stopping", "shutdown requested");
    maelys_egress_error_free(command.error);
    maelys_egress_error_free(server_error);
    return status;
}
