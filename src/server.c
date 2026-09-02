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

/* Server lifecycle and owner reactor loop: creation, listener binding,
 * policy generations, metrics snapshots, deadlines, run, stop and destroy. */

static maelys_egress_policy_t *clone_policy(const maelys_egress_policy_t *source) {
    maelys_egress_policy_t *copy = calloc(1, sizeof(*copy));
    if (!copy) return NULL;
    copy->destinations = calloc(source->count, sizeof(*copy->destinations));
    if (!copy->destinations) { free(copy); return NULL; }
    copy->capacity = source->count;
    copy->count = source->count;
    copy->sealed = 1;
    memcpy(copy->digest_hex, source->digest_hex, sizeof(copy->digest_hex));
    for (size_t i = 0; i < source->count; ++i) {
        copy->destinations[i] = source->destinations[i];
        copy->destinations[i].host = egress_strdup(source->destinations[i].host);
        if (!copy->destinations[i].host) {
            maelys_egress_policy_destroy(copy);
            return NULL;
        }
    }
    return copy;
}

static uint64_t next_deadline(const maelys_egress_server_t *server) {
    uint64_t deadline = MAELYS_SYS_DEADLINE_INFINITE;
    for (size_t i = 0; i < server->config.max_connections; ++i) {
        const egress_connection_t *connection = &server->connections[i];
        if (connection->state == CONNECTION_UNUSED) continue;
        uint64_t candidate = connection->state == CONNECTION_TLS_HANDSHAKE ||
            connection->state == CONNECTION_HANDSHAKE ||
            connection->state == CONNECTION_CONNECTING ||
            connection->state == CONNECTION_IDENTITY ? connection->handshake_deadline_ms :
            add_saturating(connection->last_activity_ms, server->config.idle_timeout_ms);
        if (candidate < deadline) deadline = candidate;
    }
    for (size_t i = 0; i < EGRESS_MAX_ADMIN_CONNECTIONS; ++i) {
        if (server->admin_connections[i].fd >= 0 &&
            server->admin_connections[i].deadline_ms < deadline) {
            deadline = server->admin_connections[i].deadline_ms;
        }
    }
    return deadline;
}

static void expire_connections(maelys_egress_server_t *server) {
    uint64_t now = monotonic_now();
    for (size_t i = 0; i < server->config.max_connections; ++i) {
        egress_connection_t *connection = &server->connections[i];
        if (connection->state == CONNECTION_UNUSED) continue;
        uint64_t deadline = connection->state == CONNECTION_TLS_HANDSHAKE ||
            connection->state == CONNECTION_HANDSHAKE ||
            connection->state == CONNECTION_CONNECTING ||
            connection->state == CONNECTION_IDENTITY ? connection->handshake_deadline_ms :
            add_saturating(connection->last_activity_ms, server->config.idle_timeout_ms);
        if (now >= deadline) {
            connection->result = MAELYS_EGRESS_ERR_TIMEOUT;
            egress_connection_close(server, i);
        }
    }
    for (size_t i = 0; i < EGRESS_MAX_ADMIN_CONNECTIONS; ++i) {
        if (server->admin_connections[i].fd >= 0 &&
            now >= server->admin_connections[i].deadline_ms) {
            egress_admin_close(server, &server->admin_connections[i]);
        }
    }
}

void egress_server_control_release(maelys_egress_server_t *server) {
    if (atomic_fetch_sub(&server->references, 1u) != 1u) return;
    (void)pthread_mutex_destroy(&server->command_lock);
    (void)pthread_mutex_destroy(&server->lifecycle_lock);
    free(server);
}

maelys_egress_result_t maelys_egress_server_create(
    const maelys_egress_policy_t *policy,
    const maelys_egress_config_t *config,
    maelys_egress_server_t **out_server,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_server) *out_server = NULL;
    if (!policy || !policy->sealed || !config || !out_server) {
        egress_set_error(out_error, "sealed policy, configuration and output are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    int loopback = !config->native_only && !config->listen_unix &&
        egress_listener_is_loopback_host(config->listen_host);
    int local_endpoint = config->native_only || config->listen_unix || loopback;
    if (!local_endpoint && !config->tls_provider) {
        egress_set_error(out_error,
            "remote listeners require an explicit TLS listener provider");
        return MAELYS_EGRESS_ERR_DENIED;
    }
    if (config->unix_principal_bound &&
        (!config->listen_unix ||
         config->unix_peer_policy != MAELYS_EGRESS_UNIX_PEER_SAME_EUID ||
         config->unix_principal_index >= config->principal_count)) {
        egress_set_error(out_error, "invalid endpoint-bound Unix principal");
        return MAELYS_EGRESS_ERR_DENIED;
    }
    if (!config->authentication_set && !config->unix_principal_bound &&
        (config->listen_unix || !config->unauthenticated_loopback || !loopback)) {
        egress_set_error(out_error,
            "authentication is mandatory unless unauthenticated loopback is explicit");
        return MAELYS_EGRESS_ERR_DENIED;
    }
    if (config->listen_unix &&
        !egress_unix_parent_is_private(config->unix_path, out_error)) {
        return MAELYS_EGRESS_ERR_DENIED;
    }
    maelys_egress_server_t *server = calloc(1, sizeof(*server));
    if (!server) return MAELYS_EGRESS_ERR_MEMORY;
    atomic_init(&server->references, 1u);
    atomic_init(&server->finalized, 0);
    if (pthread_mutex_init(&server->lifecycle_lock, NULL) != 0) {
        free(server);
        return MAELYS_EGRESS_ERR_MEMORY;
    }
    if (pthread_mutex_init(&server->command_lock, NULL) != 0) {
        (void)pthread_mutex_destroy(&server->lifecycle_lock);
        free(server);
        return MAELYS_EGRESS_ERR_MEMORY;
    }
    server->listener_fd = -1;
    server->admin_listener_fd = -1;
    for (size_t i = 0; i < EGRESS_MAX_ADMIN_CONNECTIONS; ++i) {
        server->admin_connections[i].fd = -1;
    }
    server->policy = clone_policy(policy);
    server->config = *config;
    if (server->config.tls_provider) {
        maelys_egress_tls_provider_retain(server->config.tls_provider);
    }
    if (server->config.audit) maelys_egress_audit_retain(server->config.audit);
    if (server->config.attestor) {
        maelys_egress_attestor_retain(server->config.attestor);
    }
    atomic_init(&server->policy_generation, 1u);
    server->connections = calloc(config->max_connections, sizeof(*server->connections));
    if (!server->policy || !server->connections) {
        maelys_egress_server_destroy(server);
        return MAELYS_EGRESS_ERR_MEMORY;
    }
    for (size_t i = 0; i < config->max_connections; ++i) {
        server->connections[i].client_fd = -1;
        server->connections[i].upstream_fd = -1;
    }
    if (maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &server->loop) != MAELYS_SYS_OK) {
        egress_set_error(out_error, "cannot create reactor: %s", strerror(errno));
        maelys_egress_server_destroy(server);
        return MAELYS_EGRESS_ERR_IO;
    }
    if (config->native_only) {
        server->listener_fd = -1;
    } else if (config->listen_unix) {
        if (!egress_unix_parent_is_private(config->unix_path, NULL)) {
            egress_set_error(out_error,
                "Unix listener parent changed before bind: %s", strerror(errno));
            maelys_egress_server_destroy(server);
            return MAELYS_EGRESS_ERR_DENIED;
        }
        server->listener_fd = egress_listener_create_unix(
            config, &server->unix_socket_device, &server->unix_socket_inode);
        server->unix_socket_created = server->listener_fd >= 0;
    } else {
        server->listener_fd = egress_listener_create_tcp(config, &server->bound_port);
    }
    if (!config->native_only && (server->listener_fd < 0 ||
        maelys_sys_loop_watch_fd(server->loop, server->listener_fd,
            MAELYS_SYS_INTEREST_READ, TOKEN_LISTENER, &server->listener_watch) != MAELYS_SYS_OK)) {
        if (config->listen_unix) {
            egress_set_error(out_error, "cannot bind Unix proxy listener %s: %s",
                           config->unix_path, strerror(errno));
        } else {
            egress_set_error(out_error, "cannot bind proxy listener %s:%u: %s",
                           config->listen_host, (unsigned int)config->port, strerror(errno));
        }
        maelys_egress_server_destroy(server);
        return MAELYS_EGRESS_ERR_IO;
    }
    if (config->admin_enabled) {
        maelys_egress_config_t admin = *config;
        admin.listen_unix = 0;
        (void)snprintf(admin.listen_host, sizeof(admin.listen_host), "%s",
                       config->admin_host);
        admin.port = config->admin_port;
        server->admin_listener_fd = egress_listener_create_tcp(
            &admin, &server->admin_bound_port);
        if (server->admin_listener_fd < 0 ||
            maelys_sys_loop_watch_fd(server->loop, server->admin_listener_fd,
                MAELYS_SYS_INTEREST_READ, TOKEN_ADMIN_LISTENER,
                &server->admin_listener_watch) != MAELYS_SYS_OK) {
            egress_set_error(out_error, "cannot bind admin listener %s:%u: %s",
                config->admin_host, (unsigned int)config->admin_port, strerror(errno));
            maelys_egress_server_destroy(server);
            return MAELYS_EGRESS_ERR_IO;
        }
    }
    *out_server = server;
    return MAELYS_EGRESS_OK;
}

uint16_t maelys_egress_server_port(const maelys_egress_server_t *server) {
    return server ? server->bound_port : 0u;
}

uint16_t maelys_egress_server_admin_port(const maelys_egress_server_t *server) {
    return server ? server->admin_bound_port : 0u;
}

int maelys_egress_server_is_running(const maelys_egress_server_t *server) {
    return server && atomic_load(&server->running) && !atomic_load(&server->stopping);
}

maelys_egress_result_t maelys_egress_server_replace_policy(
    maelys_egress_server_t *server, const maelys_egress_policy_t *policy,
    uint64_t *out_generation, char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_generation) *out_generation = 0u;
    if (!server || !policy || !policy->sealed) {
        if (server) (void)atomic_fetch_add(&server->metric_reload_failures, 1u);
        egress_set_error(out_error, "server and sealed replacement policy are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    maelys_egress_policy_t *copy = clone_policy(policy);
    if (!copy) {
        (void)atomic_fetch_add(&server->metric_reload_failures, 1u);
        return MAELYS_EGRESS_ERR_MEMORY;
    }
    (void)pthread_mutex_lock(&server->lifecycle_lock);
    if (atomic_load(&server->stopping)) {
        (void)pthread_mutex_unlock(&server->lifecycle_lock);
        maelys_egress_policy_destroy(copy);
        (void)atomic_fetch_add(&server->metric_reload_failures, 1u);
        egress_set_error(out_error, "cannot replace policy on a stopping server");
        return MAELYS_EGRESS_ERR_STATE;
    }
    maelys_egress_policy_t *old = server->policy;
    server->policy = copy;
    uint64_t generation = atomic_fetch_add(&server->policy_generation, 1u) + 1u;
    (void)pthread_mutex_unlock(&server->lifecycle_lock);
    maelys_egress_policy_destroy(old);
    if (out_generation) *out_generation = generation;
    return MAELYS_EGRESS_OK;
}

uint64_t maelys_egress_server_policy_generation(const maelys_egress_server_t *server) {
    return server ? atomic_load(&server->policy_generation) : 0u;
}

maelys_egress_result_t maelys_egress_server_metrics_snapshot(
    const maelys_egress_server_t *server, maelys_egress_metrics_t **out_metrics,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_metrics) *out_metrics = NULL;
    if (!server || !out_metrics) return MAELYS_EGRESS_ERR_ARGUMENT;
    maelys_egress_metrics_t *metrics = calloc(1, sizeof(*metrics));
    if (!metrics) return MAELYS_EGRESS_ERR_MEMORY;
    metrics->accepted = atomic_load(&server->metric_accepted);
    metrics->active = atomic_load(&server->metric_active);
    metrics->admitted = atomic_load(&server->metric_admitted);
    metrics->denied = atomic_load(&server->metric_denied);
    metrics->auth_failures = atomic_load(&server->metric_auth_failures);
    metrics->quota_denials = atomic_load(&server->metric_quota_denials);
    metrics->bytes_from_clients = atomic_load(&server->metric_bytes_from_clients);
    metrics->bytes_to_clients = atomic_load(&server->metric_bytes_to_clients);
    metrics->receipts = atomic_load(&server->metric_receipts);
    metrics->policy_generation = atomic_load(&server->policy_generation);
    metrics->reload_failures = atomic_load(&server->metric_reload_failures);
    metrics->audit_failures = atomic_load(&server->metric_audit_failures);
    *out_metrics = metrics;
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_server_run(
    maelys_egress_server_t *server,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!server || atomic_load(&server->stopping) ||
        atomic_exchange(&server->started, 1)) {
        egress_set_error(out_error, "server is absent, stopped or already used");
        return MAELYS_EGRESS_ERR_STATE;
    }
    atomic_store(&server->running, 1);
    maelys_egress_result_t result = MAELYS_EGRESS_OK;
    while (!atomic_load(&server->stopping)) {
        egress_open_commands_process(server);
        egress_open_commands_cancel(server);
        maelys_sys_event_t events[128];
        size_t count = 0u;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_PROGRESS;
        maelys_sys_result_t loop_result = maelys_sys_loop_step(
            server->loop, next_deadline(server), events,
            sizeof(events) / sizeof(events[0]), &count, &step);
        if (loop_result != MAELYS_SYS_OK) {
            egress_set_error(out_error, "reactor step failed: %s", strerror(errno));
            result = MAELYS_EGRESS_ERR_IO;
            break;
        }
        if (step == MAELYS_SYS_STEP_STOPPED) break;
        for (size_t i = 0; i < count; ++i) {
            if (events[i].token == TOKEN_LISTENER) {
                if (events[i].flags & MAELYS_SYS_EVENT_READ) egress_connection_accept_all(server);
                continue;
            }
            if (events[i].token == TOKEN_ADMIN_LISTENER) {
                if (events[i].flags & MAELYS_SYS_EVENT_READ) {
                    egress_admin_accept_all(server);
                }
                continue;
            }
            if (events[i].token & TOKEN_ADMIN_FLAG) {
                size_t slot;
                uint64_t generation;
                unsigned int side;
                decode_token(events[i].token & ~TOKEN_ADMIN_FLAG,
                             &slot, &generation, &side);
                (void)side;
                if (slot < EGRESS_MAX_ADMIN_CONNECTIONS &&
                    server->admin_connections[slot].fd >= 0 &&
                    server->admin_connections[slot].generation == generation) {
                    egress_admin_dispatch(server, slot, events[i].flags);
                }
                continue;
            }
            size_t slot;
            uint64_t generation;
            unsigned int side;
            decode_token(events[i].token, &slot, &generation, &side);
            if (slot >= server->config.max_connections) continue;
            egress_connection_t *connection = &server->connections[slot];
            if (connection->state == CONNECTION_UNUSED ||
                connection->generation != generation) continue;
            egress_relay_dispatch(server, slot, side, events[i].flags);
        }
        egress_open_commands_process(server);
        egress_open_commands_cancel(server);
        expire_connections(server);
    }
    egress_open_commands_reject(server, MAELYS_EGRESS_ERR_CANCELLED);
    for (size_t i = 0; i < server->config.max_connections; ++i) {
        egress_connection_t *connection = &server->connections[i];
        if (connection->state != CONNECTION_UNUSED &&
            connection->result == MAELYS_EGRESS_OK) {
            connection->result = result == MAELYS_EGRESS_OK ?
                MAELYS_EGRESS_ERR_CANCELLED : result;
        }
        if (connection->state != CONNECTION_UNUSED && connection->open_command) {
            egress_connection_close(server, i);
        }
    }
    atomic_store(&server->running, 0);
    return result;
}

maelys_egress_result_t maelys_egress_server_stop(maelys_egress_server_t *server) {
    if (!server) return MAELYS_EGRESS_ERR_ARGUMENT;
    (void)pthread_mutex_lock(&server->lifecycle_lock);
    atomic_store(&server->stopping, 1);
    maelys_egress_result_t result = maelys_sys_loop_stop(server->loop) == MAELYS_SYS_OK ?
        MAELYS_EGRESS_OK : MAELYS_EGRESS_ERR_IO;
    (void)pthread_mutex_unlock(&server->lifecycle_lock);
    return result;
}

void maelys_egress_server_destroy(maelys_egress_server_t *server) {
    if (!server) return;
    (void)pthread_mutex_lock(&server->lifecycle_lock);
    if (atomic_load(&server->running)) {
        atomic_store(&server->stopping, 1);
        (void)maelys_sys_loop_stop(server->loop);
        (void)pthread_mutex_unlock(&server->lifecycle_lock);
        return;
    }
    if (atomic_exchange(&server->finalized, 1)) {
        (void)pthread_mutex_unlock(&server->lifecycle_lock);
        return;
    }
    atomic_store(&server->stopping, 1);
    egress_open_commands_reject(server, MAELYS_EGRESS_ERR_CANCELLED);
    if (server->connections) {
        for (size_t i = 0; i < server->config.max_connections; ++i) {
            egress_connection_close(server, i);
        }
    }
    if (server->listener_watch && server->loop) {
        (void)maelys_sys_loop_unwatch(server->loop, server->listener_watch);
    }
    if (server->admin_listener_watch && server->loop) {
        (void)maelys_sys_loop_unwatch(server->loop, server->admin_listener_watch);
    }
    for (size_t i = 0; i < EGRESS_MAX_ADMIN_CONNECTIONS; ++i) {
        if (server->admin_connections[i].fd >= 0) {
            egress_admin_close(server, &server->admin_connections[i]);
        }
    }
    (void)maelys_sys_fd_close(&server->listener_fd);
    (void)maelys_sys_fd_close(&server->admin_listener_fd);
    if (server->unix_socket_created) {
        egress_listener_unlink_unix_identity(server->config.unix_path,
                             server->unix_socket_device,
                             server->unix_socket_inode);
        server->unix_socket_created = 0;
    }
    if (server->loop) (void)maelys_sys_loop_destroy(&server->loop);
    maelys_egress_policy_destroy(server->policy);
    egress_secure_zero(server->config.principals, sizeof(server->config.principals));
    maelys_egress_tls_provider_release(server->config.tls_provider);
    maelys_egress_audit_release(server->config.audit);
    maelys_egress_attestor_release(server->config.attestor);
    free(server->connections);
    server->connections = NULL;
    (void)pthread_mutex_unlock(&server->lifecycle_lock);
    egress_server_control_release(server);
}
