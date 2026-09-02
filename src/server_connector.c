#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "src/server_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native connector sessions: open commands queued from embedder threads,
 * completed on the owner reactor thread through the same admission path as
 * proxied connections. */

static void open_command_retain(egress_open_command_t *command) {
    (void)atomic_fetch_add(&command->references, 1u);
}

static void open_command_release(egress_open_command_t *command) {
    if (!command || atomic_fetch_sub(&command->references, 1u) != 1u) return;
    (void)maelys_sys_fd_close(&command->server_fd);
    (void)maelys_sys_fd_close(&command->client_fd);
    maelys_sys_condition_destroy(command->condition);
    maelys_sys_mutex_destroy(command->mutex);
    egress_secure_zero(command, sizeof(*command));
    free(command);
}

void egress_open_command_complete(
    egress_open_command_t *command, maelys_egress_result_t result) {
    if (!command) return;
    (void)maelys_sys_mutex_lock(command->mutex);
    if (!command->completed) {
        command->result = atomic_load(&command->cancelled) &&
            result == MAELYS_EGRESS_OK ? MAELYS_EGRESS_ERR_CANCELLED : result;
        command->completed = 1;
        (void)maelys_sys_condition_broadcast(command->condition);
    }
    (void)maelys_sys_mutex_unlock(command->mutex);
    open_command_release(command);
}

void egress_open_commands_process(maelys_egress_server_t *server) {
    egress_open_command_t *commands = NULL;
    (void)pthread_mutex_lock(&server->command_lock);
    commands = server->command_head;
    server->command_head = NULL;
    server->command_tail = NULL;
    (void)pthread_mutex_unlock(&server->command_lock);

    while (commands) {
        egress_open_command_t *command = commands;
        commands = command->next;
        command->next = NULL;
        if (atomic_load(&command->cancelled) || atomic_load(&server->stopping)) {
            egress_open_command_complete(command, MAELYS_EGRESS_ERR_CANCELLED);
            continue;
        }
        if (server->connection_count >= server->config.max_connections) {
            egress_open_command_complete(command, MAELYS_EGRESS_ERR_STATE);
            continue;
        }
        size_t slot = 0u;
        while (slot < server->config.max_connections &&
               server->connections[slot].state != CONNECTION_UNUSED) ++slot;
        if (slot == server->config.max_connections) {
            egress_open_command_complete(command, MAELYS_EGRESS_ERR_STATE);
            continue;
        }
        egress_connection_t *connection = &server->connections[slot];
        uint64_t generation = ++server->next_id;
        memset(connection, 0, sizeof(*connection));
        connection->state = CONNECTION_HANDSHAKE;
        connection->generation = generation;
        connection->client_fd = command->server_fd;
        command->server_fd = -1;
        connection->upstream_fd = -1;
        connection->principal_index = command->principal_index;
        connection->protocol = MAELYS_EGRESS_PROTOCOL_CONNECTOR;
        connection->open_command = command;
        connection->client_to_upstream.capacity = server->config.buffer_bytes;
        connection->upstream_to_client.capacity = server->config.buffer_bytes;
        connection->client_to_upstream.data = malloc(server->config.buffer_bytes);
        connection->upstream_to_client.data = malloc(server->config.buffer_bytes);
        connection->started_mono_ms = monotonic_now();
        connection->started_unix_ms = wall_now();
        connection->last_activity_ms = connection->started_mono_ms;
        connection->handshake_deadline_ms = add_saturating(
            connection->started_mono_ms, server->config.handshake_timeout_ms);
        ++server->connection_count;
        (void)atomic_fetch_add(&server->metric_accepted, 1u);
        (void)atomic_fetch_add(&server->metric_active, 1u);
        if (!connection->client_to_upstream.data ||
            !connection->upstream_to_client.data) {
            connection->result = MAELYS_EGRESS_ERR_MEMORY;
            egress_connection_close(server, slot);
            continue;
        }
        egress_proxy_request_t request;
        memset(&request, 0, sizeof(request));
        request.protocol = MAELYS_EGRESS_PROTOCOL_CONNECTOR;
        request.port = command->port;
        request.principal_index = command->principal_index;
        (void)snprintf(request.host, sizeof(request.host), "%s", command->host);
        (void)snprintf(request.invocation_id, sizeof(request.invocation_id), "%s",
                       command->invocation_id);
        if (!egress_connection_begin_request(server, slot, &request) &&
            connection->state != CONNECTION_UNUSED) {
            connection->result = MAELYS_EGRESS_ERR_IO;
            egress_connection_close(server, slot);
        }
    }
}

void egress_open_commands_cancel(maelys_egress_server_t *server) {
    for (size_t i = 0; i < server->config.max_connections; ++i) {
        egress_connection_t *connection = &server->connections[i];
        if (connection->state != CONNECTION_UNUSED && connection->open_command &&
            atomic_load(&connection->open_command->cancelled)) {
            connection->result = MAELYS_EGRESS_ERR_CANCELLED;
            egress_connection_close(server, i);
        }
    }
}

void egress_open_commands_reject(
    maelys_egress_server_t *server, maelys_egress_result_t result) {
    egress_open_command_t *commands = NULL;
    (void)pthread_mutex_lock(&server->command_lock);
    commands = server->command_head;
    server->command_head = NULL;
    server->command_tail = NULL;
    (void)pthread_mutex_unlock(&server->command_lock);
    while (commands) {
        egress_open_command_t *command = commands;
        commands = command->next;
        command->next = NULL;
        egress_open_command_complete(command, result);
    }
}

maelys_egress_result_t egress_server_connector_bind(
    maelys_egress_server_t *server,
    const char *username,
    const char *secret,
    size_t *out_principal_index,
    char out_principal[EGRESS_MAX_USERNAME + 1u],
    char out_invocation_id[EGRESS_MAX_INVOCATION_ID + 1u],
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!server || !username || !secret || !out_principal_index ||
        !out_principal || !out_invocation_id) {
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    (void)pthread_mutex_lock(&server->lifecycle_lock);
    if (atomic_load(&server->finalized) || atomic_load(&server->stopping)) {
        (void)pthread_mutex_unlock(&server->lifecycle_lock);
        egress_set_error(out_error, "cannot bind a connector to a stopping server");
        return MAELYS_EGRESS_ERR_STATE;
    }
    size_t principal_index = SIZE_MAX;
    char invocation_id[EGRESS_MAX_INVOCATION_ID + 1u] = {0};
    int authenticated = egress_credentials_lookup(
        &server->config, username, secret, invocation_id, &principal_index);
    if (!authenticated || principal_index >= server->config.principal_count) {
        (void)atomic_fetch_add(&server->metric_auth_failures, 1u);
        (void)pthread_mutex_unlock(&server->lifecycle_lock);
        egress_set_error(out_error, "connector authentication failed");
        return MAELYS_EGRESS_ERR_DENIED;
    }
    (void)atomic_fetch_add(&server->references, 1u);
    *out_principal_index = principal_index;
    (void)snprintf(out_principal, EGRESS_MAX_USERNAME + 1u, "%s",
                   server->config.principals[principal_index].username);
    (void)snprintf(out_invocation_id, EGRESS_MAX_INVOCATION_ID + 1u, "%s",
                   invocation_id);
    (void)pthread_mutex_unlock(&server->lifecycle_lock);
    return MAELYS_EGRESS_OK;
}

void egress_server_connector_release(maelys_egress_server_t *server) {
    if (server) egress_server_control_release(server);
}

static int server_loop_wake_if_live(maelys_egress_server_t *server) {
    int woke = 0;
    (void)pthread_mutex_lock(&server->lifecycle_lock);
    if (!atomic_load(&server->finalized) && server->loop) {
        woke = maelys_sys_loop_wake(server->loop) == MAELYS_SYS_OK;
    }
    (void)pthread_mutex_unlock(&server->lifecycle_lock);
    return woke;
}

maelys_egress_result_t egress_server_open_stream(
    maelys_egress_server_t *server,
    size_t principal_index,
    const char *principal,
    const char *invocation_id,
    const char *canonical_host,
    uint16_t port,
    uint64_t timeout_ms,
    int *out_fd,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_fd) *out_fd = -1;
    char normalized[EGRESS_MAX_HOST + 1u];
    if (!server || !principal || !invocation_id || !canonical_host || !port ||
        !timeout_ms || !out_fd ||
        !egress_canonical_host(canonical_host, normalized) ||
        strcmp(normalized, canonical_host) != 0) {
        egress_set_error(out_error,
            "a live connector, canonical host, port and finite timeout are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    if (atomic_load(&server->stopping) || atomic_load(&server->finalized)) {
        egress_set_error(out_error, "connector server is not running");
        return MAELYS_EGRESS_ERR_STATE;
    }
    egress_open_command_t *command = calloc(1, sizeof(*command));
    if (!command) return MAELYS_EGRESS_ERR_MEMORY;
    command->server_fd = -1;
    command->client_fd = -1;
    atomic_init(&command->references, 1u);
    atomic_init(&command->cancelled, 0);
    if (maelys_sys_mutex_create(&command->mutex) != MAELYS_SYS_OK ||
        maelys_sys_condition_create(&command->condition) != MAELYS_SYS_OK ||
        !egress_listener_create_private_tcp_pair(&command->server_fd, &command->client_fd)) {
        open_command_release(command);
        egress_set_error(out_error, "cannot create private connector stream: %s",
                       strerror(errno));
        return MAELYS_EGRESS_ERR_IO;
    }
    command->principal_index = principal_index;
    command->port = port;
    (void)snprintf(command->principal, sizeof(command->principal), "%s", principal);
    (void)snprintf(command->invocation_id, sizeof(command->invocation_id), "%s",
                   invocation_id);
    (void)snprintf(command->host, sizeof(command->host), "%s", normalized);
    (void)pthread_mutex_lock(&server->command_lock);
    if (atomic_load(&server->stopping) || atomic_load(&server->finalized)) {
        (void)pthread_mutex_unlock(&server->command_lock);
        open_command_release(command);
        egress_set_error(out_error, "connector session open cancelled");
        return MAELYS_EGRESS_ERR_CANCELLED;
    }
    open_command_retain(command);
    if (server->command_tail) server->command_tail->next = command;
    else server->command_head = command;
    server->command_tail = command;
    (void)pthread_mutex_unlock(&server->command_lock);

    uint64_t now = monotonic_now();
    uint64_t deadline = add_saturating(now, timeout_ms);
    if (!server_loop_wake_if_live(server)) {
        atomic_store(&command->cancelled, 1);
    }
    maelys_egress_result_t result = MAELYS_EGRESS_ERR_IO;
    (void)maelys_sys_mutex_lock(command->mutex);
    while (!command->completed) {
        maelys_sys_result_t waited = maelys_sys_condition_wait_until(
            command->condition, command->mutex, deadline);
        if (waited == MAELYS_SYS_ERR_TIMEOUT) {
            atomic_store(&command->cancelled, 1);
            result = MAELYS_EGRESS_ERR_TIMEOUT;
            break;
        }
        if (waited != MAELYS_SYS_OK) {
            atomic_store(&command->cancelled, 1);
            result = MAELYS_EGRESS_ERR_IO;
            break;
        }
    }
    if (command->completed) result = command->result;
    if (result == MAELYS_EGRESS_OK) {
        *out_fd = command->client_fd;
        command->client_fd = -1;
    }
    (void)maelys_sys_mutex_unlock(command->mutex);
    if (result != MAELYS_EGRESS_OK) {
        (void)server_loop_wake_if_live(server);
        egress_set_error(out_error, result == MAELYS_EGRESS_ERR_DENIED ?
            "connector destination denied" : result == MAELYS_EGRESS_ERR_TIMEOUT ?
            "connector session open timed out" : result == MAELYS_EGRESS_ERR_CANCELLED ?
            "connector session open cancelled" : "connector session open failed");
    }
    open_command_release(command);
    return result;
}
