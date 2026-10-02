/* SPDX-License-Identifier: MPL-2.0 */
#include "cli/cli.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "maelys/sys/clock.h"

/* `channel exec`: one server without a listener, one channel, one program
 * that inherits the channel's client end, and that program's exit status.
 * The contract is proposals/egress-channel-exec.md.
 *
 * The main thread owns the server's reactor. This coordinator creates the
 * channel, starts the program and waits for it. The program is started
 * through the framework, which holds the checked executable open through
 * exec and installs the descriptor; nothing here forks.
 *
 * Shutdown has one order: stop the server, which cancels the opens in
 * flight and ends the connections it relays when its loop returns; then,
 * on the thread that owns the server, destroy the channel, release the
 * connector and destroy the server. Since stopping ends the connections,
 * a server that stops by itself while the program runs needs nothing from
 * this file but a word to the operator: the program learns it from its
 * connections, and its status is still waited for. */

typedef struct exec_command {
    signal_context_t signals;
    egress_cli_exec_t *exec;
    maelys_cli_error_t *error;
    maelys_egress_connector_t *connector;   /* released by the owner thread */
    maelys_egress_channel_t *channel;       /* destroyed by the owner thread */
    atomic_int started;                     /* the program runs or has run */
    atomic_int finished;                    /* the coordinator asked for the stop */
    atomic_int server_finished;             /* server_run has returned */
} exec_command_t;

static void pause_briefly(void) {
    struct timespec delay = {.tv_nsec = 10000000L};
    (void)nanosleep(&delay, NULL);
}

/* The program's environment: the launcher's own, plus where the channel is
 * and how long a connect through it may take. */
static char **program_environment(const egress_cli_settings_t *settings) {
    maelys_cli_environment_t overlay = {0};
    char assignment[96];
    char **envp = NULL;
    (void)snprintf(assignment, sizeof(assignment), "MAELYS_EGRESS_CHANNEL_FD=%d",
        settings->channel_fd);
    int failed = maelys_cli_environment_append(&overlay, assignment) != 0;
    (void)snprintf(assignment, sizeof(assignment),
        "MAELYS_EGRESS_CHANNEL_CONNECT_TIMEOUT_MS=%llu",
        (unsigned long long)settings->channel_connect_timeout_ms);
    failed = failed || maelys_cli_environment_append(&overlay, assignment) != 0;
    if (failed || maelys_cli_environment_to_envp_inherited(&overlay, &envp) != 0) envp = NULL;
    maelys_cli_environment_clear(&overlay);
    return envp;
}

static void *exec_command_main(void *opaque) {
    exec_command_t *command = opaque;
    const egress_cli_settings_t *settings = command->signals.baseline;
    maelys_egress_server_t *server = command->signals.server;
    egress_cli_exec_t *exec = command->exec;
    maelys_cli_process_t *program = NULL;
    char **envp = NULL;
    char *detail = NULL;
    int client_fd = -1;
    pthread_t signal_thread;
    int signal_started = 0;
    uint64_t deadline = 0u, now = 0u;

    if (maelys_sys_deadline_after(5000u, &deadline) != MAELYS_SYS_OK) {
        maelys_cli_error_set(command->error, MAELYS_CLI_CODE_UNEXPECTED, NULL,
            "cannot read the monotonic clock");
        goto done;
    }
    while (!maelys_egress_server_is_running(server)) {
        if (atomic_load(&command->server_finished) ||
            maelys_sys_monotonic_ms(&now) != MAELYS_SYS_OK || now >= deadline) {
            /* The server's own error, if it has one, replaces this. */
            maelys_cli_error_set(command->error, MAELYS_CLI_CODE_IO_FAILED,
                "Inspect the configuration and process resources.",
                "the server did not start");
            goto done;
        }
        pause_briefly();
    }
    maelys_egress_result_t result =
        maelys_egress_server_native_connector_create(server, &command->connector, &detail);
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_channel_create(command->connector,
        settings->channel_connect_timeout_ms, &command->channel, &client_fd, &detail);
    if (result != MAELYS_EGRESS_OK) {
        maelys_cli_error_set(command->error, MAELYS_CLI_CODE_IO_FAILED,
            "Check process and descriptor limits.", "%s",
            detail ? detail : maelys_egress_result_string(result));
        goto done;
    }
    envp = program_environment(settings);
    if (!envp) {
        maelys_cli_error_set(command->error, MAELYS_CLI_CODE_UNEXPECTED, NULL,
            "cannot build the program's environment");
        goto done;
    }
    const maelys_cli_process_inherit_t inherit[] = {{client_fd, settings->channel_fd}};
    /* By the descriptor that was checked unless the configuration asks for
     * the path: the object executed is then the object checked, with no
     * window. A multi-call binary that names itself from how it was executed
     * cannot run that way, and channel_exec_by_path is for it alone. */
    const maelys_cli_process_options_t options = {
        .inherit = inherit, .inherit_count = 1u,
        .exec_by_path = settings->channel_exec_by_path
    };
    if (maelys_cli_process_start(exec->argv[0], exec->argv, envp, &options, &program) != 0) {
        int saved = errno;
        maelys_cli_error_set(command->error, maelys_cli_file_error_code(saved),
            "Check that the program can be executed by this user.",
            "%s: %s", exec->argv[0], strerror(saved));
        program = NULL;
        goto done;
    }
    /* From here the exit status is the program's. The launcher keeps no
     * copy of the client end: when the program and what it started have
     * closed theirs, the channel sees its peer gone. */
    (void)close(client_fd);
    client_fd = -1;
    exec->started = 1;
    atomic_store(&command->started, 1);
    command->signals.program = program;
    if (pthread_create(&signal_thread, NULL, egress_cli_signal_main, &command->signals) == 0) {
        signal_started = 1;
    } else {
        egress_cli_diagnostic("cannot start the signal thread: signals are not forwarded");
    }
    maelys_cli_process_status_t status;
    if (maelys_cli_process_wait(program, &status) == 0) {
        exec->exit_code = maelys_cli_process_exit_code(&status);
    } else {
        egress_cli_diagnostic("cannot wait for the program: %s", strerror(errno));
        exec->exit_code = MAELYS_CLI_EXIT_FAILURE;
    }
done:
    atomic_store(&command->signals.stopping, 1);
    if (signal_started) {
        (void)pthread_kill(signal_thread, SIGTERM);
        (void)pthread_join(signal_thread, NULL);
    }
    /* Said before the stop is asked, so that the owner thread can tell a
     * stop it was not asked for from this one. */
    atomic_store(&command->finished, 1);
    (void)maelys_egress_server_stop(server);
    if (client_fd >= 0) (void)close(client_fd);
    maelys_cli_process_release(program);
    maelys_cli_envp_free(envp);
    maelys_egress_error_free(detail);
    return NULL;
}

int egress_cli_exec_run(maelys_egress_server_t *server,
    const egress_cli_settings_t *settings, const char *path, const sigset_t *signals,
    egress_cli_exec_t *exec, maelys_cli_error_t *error) {
    exec_command_t command = {
        .signals = {.server = server, .signals = *signals, .config_path = path,
                    .baseline = settings},
        .exec = exec, .error = error
    };
    atomic_init(&command.signals.stopping, 0);
    atomic_init(&command.started, 0);
    atomic_init(&command.finished, 0);
    atomic_init(&command.server_finished, 0);
    memset(error, 0, sizeof(*error));
    pthread_t worker;
    if (pthread_create(&worker, NULL, exec_command_main, &command) != 0) {
        maelys_cli_error_set(error, MAELYS_CLI_CODE_UNEXPECTED,
            "Check available process/thread resources.", "cannot create the launch coordinator");
        return -1;
    }
    char *server_error = NULL;
    maelys_egress_result_t result = maelys_egress_server_run(server, &server_error);
    atomic_store(&command.server_finished, 1);
    const char *reason = server_error ? server_error : result != MAELYS_EGRESS_OK ?
        maelys_egress_result_string(result) : "it was stopped";
    /* Nothing here asked for this stop: the program is not signalled. Its
     * connections ended when the loop returned, which is how it is told,
     * and its status is still the one reported. */
    int alone = !atomic_load(&command.finished);
    int told = alone && atomic_load(&command.started);
    if (told) egress_cli_diagnostic("the server stopped while the program was running: %s", reason);
    (void)pthread_join(worker, NULL);
    /* Started after the look above. */
    if (exec->started && alone && !told) {
        egress_cli_diagnostic("the server stopped while the program was running: %s", reason);
    }
    maelys_egress_channel_destroy(command.channel);
    maelys_egress_connector_release(command.connector);
    int status = 0;
    if (!exec->started) {
        if (result != MAELYS_EGRESS_OK) {
            maelys_cli_error_set(error, MAELYS_CLI_CODE_IO_FAILED,
                "Inspect the configuration, referenced files and process resources.",
                "%s", reason);
        }
        status = -1;
    }
    maelys_egress_error_free(server_error);
    return status;
}
