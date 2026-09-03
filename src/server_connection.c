#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "src/server_internal.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* Connection admission and state machine up to the relay: accept, proxy
 * handshake (HTTP/SOCKS), policy match, quota admission, pinned upstream
 * connect, tunnel identity guard, failure responses and slot release. */

void egress_connection_close(maelys_egress_server_t *server, size_t slot) {
    egress_connection_t *connection = &server->connections[slot];
    if (connection->state == CONNECTION_UNUSED) return;
    if (connection->open_command) {
        maelys_egress_result_t open_result = connection->result == MAELYS_EGRESS_OK ?
            MAELYS_EGRESS_ERR_IO : connection->result;
        egress_open_command_t *command = connection->open_command;
        connection->open_command = NULL;
        egress_open_command_complete(command, open_result);
    }
    if (connection->client_watch) {
        (void)maelys_sys_loop_unwatch(server->loop, connection->client_watch);
    }
    if (connection->upstream_watch) {
        (void)maelys_sys_loop_unwatch(server->loop, connection->upstream_watch);
    }
    if (connection->tls_session && server->config.tls_provider) {
        server->config.tls_provider->ops.session_release(
            server->config.tls_provider->context, connection->tls_session);
        connection->tls_session = NULL;
    }
    egress_socket_release(&connection->client_socket, &connection->client_fd);
    egress_socket_release(&connection->upstream_socket, &connection->upstream_fd);
    if (connection->quota_admitted &&
        connection->principal_index < server->config.principal_count &&
        server->principal_active[connection->principal_index]) {
        --server->principal_active[connection->principal_index];
    }
    (void)atomic_fetch_add(&server->metric_bytes_from_clients,
                           connection->bytes_from_client);
    (void)atomic_fetch_add(&server->metric_bytes_to_clients,
                           connection->bytes_to_client);
    if (connection->result == MAELYS_EGRESS_ERR_DENIED) {
        (void)atomic_fetch_add(&server->metric_denied, 1u);
    }
    egress_receipt_emit(server, connection);
    free(connection->client_to_upstream.data);
    free(connection->upstream_to_client.data);
    uint64_t generation = connection->generation;
    memset(connection, 0, sizeof(*connection));
    connection->client_fd = -1;
    connection->upstream_fd = -1;
    connection->generation = generation;
    if (server->connection_count) --server->connection_count;
    (void)atomic_fetch_sub(&server->metric_active, 1u);
}

static int queue_to_client(egress_connection_t *connection,
                           const void *bytes, size_t length) {
    if (!buffer_append(&connection->upstream_to_client, bytes, length)) return 0;
    connection->quota_client_exempt_bytes = add_saturating(
        connection->quota_client_exempt_bytes, (uint64_t)length);
    return 1;
}

void egress_connection_fail(
    maelys_egress_server_t *server,
    size_t slot,
    maelys_egress_result_t result,
    int authentication_failure) {
    egress_connection_t *connection = &server->connections[slot];
    if (authentication_failure) {
        (void)atomic_fetch_add(&server->metric_auth_failures, 1u);
    }
    if (connection->result == MAELYS_EGRESS_OK) connection->result = result;
    connection->close_after_flush = 1;
    if (connection->upstream_watch) {
        (void)maelys_sys_loop_unwatch(server->loop, connection->upstream_watch);
        connection->upstream_watch = 0u;
    }
    egress_socket_release(&connection->upstream_socket, &connection->upstream_fd);
    if (connection->protocol == MAELYS_EGRESS_PROTOCOL_CONNECTOR) {
        egress_connection_close(server, slot);
        return;
    } else if (connection->protocol == MAELYS_EGRESS_PROTOCOL_SOCKS5) {
        unsigned char response[10] = {5u, result == MAELYS_EGRESS_ERR_DENIED ? 2u : 1u,
                                      0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u};
        (void)queue_to_client(connection, response, sizeof(response));
    } else {
        const char *response = authentication_failure ?
            "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"maelys-egress\"\r\nConnection: close\r\nContent-Length: 0\r\n\r\n" :
            result == MAELYS_EGRESS_ERR_DENIED ?
            "HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n" :
            result == MAELYS_EGRESS_ERR_PROTOCOL ?
            "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n" :
            "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        (void)queue_to_client(connection, response, strlen(response));
    }
}

static int watch_or_modify(
    maelys_egress_server_t *server,
    int fd,
    unsigned interests,
    uint64_t token,
    maelys_sys_watch_t *watch) {
    if (*watch) {
        return maelys_sys_loop_modify(server->loop, *watch, interests) == MAELYS_SYS_OK;
    }
    return maelys_sys_loop_watch_fd(server->loop, fd, interests, token, watch) ==
        MAELYS_SYS_OK;
}

int egress_connection_update_watches(maelys_egress_server_t *server, size_t slot) {
    egress_connection_t *connection = &server->connections[slot];
    if (connection->state == CONNECTION_UNUSED) return 1;
    unsigned client_interests = 0u;
    if (connection->state == CONNECTION_TLS_HANDSHAKE) {
        client_interests = connection->tls_handshake_interest;
    } else {
        if (!connection->close_after_flush && !connection->quota_exhausted &&
            !connection->client_eof &&
            buffer_available(&connection->client_to_upstream) > 0u) {
            client_interests |= connection->tls_session && connection->tls_read_interest ?
                connection->tls_read_interest : MAELYS_SYS_INTEREST_READ;
        }
        if (connection->upstream_to_client.length) {
            client_interests |= connection->tls_session && connection->tls_write_interest ?
                connection->tls_write_interest : MAELYS_SYS_INTEREST_WRITE;
        }
        if (connection->tls_shutdown_interest) {
            client_interests |= connection->tls_shutdown_interest;
        }
    }
    if (!client_interests) {
        if (connection->client_watch) {
            (void)maelys_sys_loop_unwatch(server->loop, connection->client_watch);
            connection->client_watch = 0u;
        }
    } else if (!watch_or_modify(server, connection->client_fd, client_interests,
        connection_token(slot, connection->generation, TOKEN_SIDE_CLIENT),
        &connection->client_watch)) return 0;

    if (connection->upstream_fd < 0) return 1;
    unsigned upstream_interests = 0u;
    if (connection->state == CONNECTION_CONNECTING) {
        upstream_interests = MAELYS_SYS_INTEREST_WRITE;
    } else if (connection->state == CONNECTION_RELAY) {
        if (!connection->quota_exhausted && !connection->upstream_eof &&
            buffer_available(&connection->upstream_to_client) > 0u) {
            upstream_interests |= MAELYS_SYS_INTEREST_READ;
        }
        if (connection->client_to_upstream.length) {
            upstream_interests |= MAELYS_SYS_INTEREST_WRITE;
        }
    }
    if (!upstream_interests) {
        if (connection->upstream_watch) {
            (void)maelys_sys_loop_unwatch(server->loop, connection->upstream_watch);
            connection->upstream_watch = 0u;
        }
    } else if (!watch_or_modify(server, connection->upstream_fd, upstream_interests,
        connection_token(slot, connection->generation, TOKEN_SIDE_UPSTREAM),
        &connection->upstream_watch)) return 0;
    return 1;
}

int egress_connection_enforce_tunnel_identity(
    maelys_egress_server_t *server, size_t slot) {
    egress_connection_t *connection = &server->connections[slot];
    if (connection->state != CONNECTION_IDENTITY ||
        !connection->client_to_upstream.length) return 1;
    char *error = NULL;
    int match = egress_tls_client_hello_matches(
        connection->client_to_upstream.data + connection->client_to_upstream.offset,
        connection->client_to_upstream.length, connection->host, &error);
    maelys_egress_error_free(error);
    if (match == 0) return 1;
    if (match < 0) {
        egress_connection_fail(server, slot, MAELYS_EGRESS_ERR_DENIED, 0);
        return egress_connection_update_watches(server, slot);
    }
    connection->tls_sni_verified = 1;
    connection->state = CONNECTION_RELAY;
    connection->last_activity_ms = monotonic_now();
    return egress_connection_update_watches(server, slot);
}

void egress_connection_connected(maelys_egress_server_t *server, size_t slot) {
    egress_connection_t *connection = &server->connections[slot];
    int tunnel = connection->protocol == MAELYS_EGRESS_PROTOCOL_HTTP_CONNECT ||
        connection->protocol == MAELYS_EGRESS_PROTOCOL_SOCKS5 ||
        connection->protocol == MAELYS_EGRESS_PROTOCOL_CONNECTOR;
    connection->state = tunnel && connection->destination->require_tls_sni ?
        CONNECTION_IDENTITY : CONNECTION_RELAY;
    connection->last_activity_ms = monotonic_now();
    if (connection->protocol == MAELYS_EGRESS_PROTOCOL_HTTP_CONNECT) {
        static const char response[] =
            "HTTP/1.1 200 Connection Established\r\nProxy-Agent: maelys-egress\r\n\r\n";
        if (!queue_to_client(connection, response, sizeof(response) - 1u)) {
            connection->result = MAELYS_EGRESS_ERR_MEMORY;
            egress_connection_close(server, slot);
            return;
        }
    } else if (connection->protocol == MAELYS_EGRESS_PROTOCOL_SOCKS5) {
        static const unsigned char response[10] = {5u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u};
        if (!queue_to_client(connection, response, sizeof(response))) {
            connection->result = MAELYS_EGRESS_ERR_MEMORY;
            egress_connection_close(server, slot);
            return;
        }
    }
    if (!egress_connection_enforce_tunnel_identity(server, slot) ||
        (connection->state != CONNECTION_UNUSED && !egress_connection_update_watches(server, slot))) {
        egress_connection_close(server, slot);
        return;
    }
    if (connection->state != CONNECTION_UNUSED && connection->open_command) {
        if (atomic_load(&connection->open_command->cancelled)) {
            connection->result = MAELYS_EGRESS_ERR_CANCELLED;
            egress_connection_close(server, slot);
            return;
        }
        egress_open_command_t *command = connection->open_command;
        connection->open_command = NULL;
        egress_open_command_complete(command, MAELYS_EGRESS_OK);
    }
}

int egress_connection_connect_next(maelys_egress_server_t *server, size_t slot) {
    egress_connection_t *connection = &server->connections[slot];
    while (connection->address_index < connection->destination->address_count) {
        const egress_address_t *address =
            &connection->destination->addresses[connection->address_index++];
        maelys_sys_socket_t *upstream = NULL;
        if (maelys_sys_socket_create(address->storage.ss_family, SOCK_STREAM,
                                     IPPROTO_TCP, &upstream) != MAELYS_SYS_OK) continue;
        connection->upstream_socket = upstream;
        connection->upstream_fd = maelys_sys_socket_native_fd(upstream);
        maelys_sys_connect_state_t state = MAELYS_SYS_CONNECT_IN_PROGRESS;
        maelys_sys_result_t started = maelys_sys_socket_connect_start(
            upstream, (const struct sockaddr *)&address->storage, address->length, &state);
        if (started == MAELYS_SYS_OK && state == MAELYS_SYS_CONNECT_CONNECTED) {
            egress_connection_connected(server, slot);
            return 1;
        }
        if (started == MAELYS_SYS_OK) {
            connection->state = CONNECTION_CONNECTING;
            return egress_connection_update_watches(server, slot);
        }
        egress_socket_release(&connection->upstream_socket, &connection->upstream_fd);
    }
    egress_connection_fail(server, slot, MAELYS_EGRESS_ERR_IO, 0);
    return egress_connection_update_watches(server, slot);
}

int egress_connection_begin_request(
    maelys_egress_server_t *server,
    size_t slot,
    const egress_proxy_request_t *request) {
    egress_connection_t *connection = &server->connections[slot];
    connection->protocol = request->protocol;
    connection->port = request->port;
    (void)snprintf(connection->host, sizeof(connection->host), "%s", request->host);
    (void)snprintf(connection->invocation_id, sizeof(connection->invocation_id),
                   "%s", request->invocation_id);
    connection->principal_index = request->principal_index;
    if (request->principal_index < server->config.principal_count) {
        const egress_principal_t *principal =
            &server->config.principals[request->principal_index];
        (void)snprintf(connection->principal, sizeof(connection->principal), "%s",
                       principal->username);
        connection->quota_bytes = principal->max_bytes_per_connection;
        connection->quota_total_bytes = principal->max_bytes_total;
        connection->quota_total_before =
            server->principal_bytes_total[request->principal_index];
        if (principal->max_active_connections &&
            server->principal_active[request->principal_index] >=
                principal->max_active_connections) {
            connection->quota_scope = MAELYS_EGRESS_QUOTA_ACTIVE_CONNECTIONS;
            (void)atomic_fetch_add(&server->metric_quota_denials, 1u);
            egress_connection_fail(server, slot, MAELYS_EGRESS_ERR_DENIED, 0);
            return egress_connection_update_watches(server, slot);
        }
    }
    (void)pthread_mutex_lock(&server->lifecycle_lock);
    const egress_destination_t *selected =
        egress_policy_find(server->policy, request->host, request->port);
    if (selected) {
        connection->destination_snapshot = *selected;
        connection->destination_snapshot.host = connection->host;
        connection->destination = &connection->destination_snapshot;
        (void)snprintf(connection->policy_digest_hex,
            sizeof(connection->policy_digest_hex), "%s", server->policy->digest_hex);
        connection->policy_generation = atomic_load(&server->policy_generation);
    }
    (void)pthread_mutex_unlock(&server->lifecycle_lock);
    if (!selected) {
        egress_connection_fail(server, slot, MAELYS_EGRESS_ERR_DENIED, 0);
        return egress_connection_update_watches(server, slot);
    }
    if (request->principal_index < server->config.principal_count) {
        ++server->principal_active[request->principal_index];
        connection->quota_admitted = 1;
    }
    (void)atomic_fetch_add(&server->metric_admitted, 1u);
    egress_buffer_t *input = &connection->client_to_upstream;
    if (request->consumed > input->length) return 0;
    size_t extra_length = input->length - request->consumed;
    if (request->forward_length + extra_length > input->capacity) return 0;
    if (extra_length) {
        memmove(input->data + request->forward_length,
                input->data + input->offset + request->consumed, extra_length);
    }
    if (request->forward_length) {
        memcpy(input->data, request->forward_bytes, request->forward_length);
    }
    input->offset = 0u;
    input->length = request->forward_length + extra_length;
    maelys_egress_quota_scope_t quota_scope = MAELYS_EGRESS_QUOTA_NONE;
    uint64_t allowance = egress_quota_allowance(server, connection, &quota_scope);
    if ((uint64_t)input->length > allowance ||
        (allowance == 0u &&
         (connection->quota_bytes || connection->quota_total_bytes))) {
        egress_quota_deny(server, connection, quota_scope);
        egress_connection_fail(server, slot, MAELYS_EGRESS_ERR_DENIED, 0);
        return egress_connection_update_watches(server, slot);
    }
    egress_quota_charge(server, connection, (uint64_t)input->length);
    return egress_connection_connect_next(server, slot);
}

static int parse_socks(
    maelys_egress_server_t *server,
    size_t slot) {
    egress_connection_t *connection = &server->connections[slot];
    egress_buffer_t *input = &connection->client_to_upstream;
    for (;;) {
        egress_proxy_request_t request;
        size_t consumed = 0u;
        unsigned char response[10] = {0};
        size_t response_length = 0u;
        int parsed = egress_parse_socks_frame(
            input->data + input->offset, input->length, &server->config,
            &connection->socks_phase, &consumed, response, &response_length,
            &request, connection->authenticated_invocation_id,
            &connection->principal_index);
        if (parsed == 0) return 1;
        if (response_length &&
            !queue_to_client(connection, response, response_length)) return 0;
        if (parsed == 2) return egress_connection_begin_request(server, slot, &request);
        if (consumed) buffer_consume(input, consumed);
        if (parsed < 0) {
            connection->result = parsed == -2 ?
                MAELYS_EGRESS_ERR_DENIED : MAELYS_EGRESS_ERR_PROTOCOL;
            connection->close_after_flush = 1;
            return 1;
        }
    }
}

int egress_connection_process_handshake(maelys_egress_server_t *server, size_t slot) {
    egress_connection_t *connection = &server->connections[slot];
    egress_buffer_t *input = &connection->client_to_upstream;
    if (!input->length) return 1;
    if (connection->protocol == MAELYS_EGRESS_PROTOCOL_SOCKS5 ||
        input->data[input->offset] == 5u) {
        connection->protocol = MAELYS_EGRESS_PROTOCOL_SOCKS5;
        int parsed = parse_socks(server, slot);
        if (!parsed && connection->state != CONNECTION_UNUSED) {
            egress_connection_fail(server, slot, MAELYS_EGRESS_ERR_PROTOCOL, 0);
        }
        return parsed;
    }
    egress_proxy_request_t request;
    memset(&request, 0, sizeof(request));
    char *parse_error = NULL;
    int parsed = egress_parse_http_request(input->data + input->offset,
        input->length, &server->config, &request, &parse_error);
    maelys_egress_error_free(parse_error);
    if (parsed == 0) return 1;
    if (parsed < 0) {
        connection->protocol = MAELYS_EGRESS_PROTOCOL_HTTP_CONNECT;
        egress_connection_fail(server, slot,
            parsed == -2 ? MAELYS_EGRESS_ERR_DENIED : MAELYS_EGRESS_ERR_PROTOCOL,
            parsed == -2);
        egress_proxy_request_clear(&request);
        return 1;
    }
    int result = egress_connection_begin_request(server, slot, &request);
    egress_proxy_request_clear(&request);
    return result;
}

void egress_connection_accept_all(maelys_egress_server_t *server) {
    for (;;) {
        maelys_sys_socket_t *client = NULL;
        if (maelys_sys_socket_accept(server->listener_socket, NULL, NULL, &client) !=
            MAELYS_SYS_OK) {
            return;
        }
        if (!egress_listener_unix_peer_allowed(server, maelys_sys_socket_native_fd(client))) {
            (void)maelys_sys_socket_release(&client);
            continue;
        }
        (void)atomic_fetch_add(&server->metric_accepted, 1u);
        if (server->connection_count >= server->config.max_connections) {
            (void)maelys_sys_socket_release(&client);
            continue;
        }
        size_t slot = 0u;
        while (slot < server->config.max_connections &&
               server->connections[slot].state != CONNECTION_UNUSED) ++slot;
        if (slot == server->config.max_connections) {
            (void)maelys_sys_socket_release(&client);
            continue;
        }
        egress_connection_t *connection = &server->connections[slot];
        uint64_t generation = ++server->next_id;
        memset(connection, 0, sizeof(*connection));
        connection->state = server->config.tls_provider ?
            CONNECTION_TLS_HANDSHAKE : CONNECTION_HANDSHAKE;
        connection->generation = generation;
        connection->client_socket = client;
        connection->client_fd = maelys_sys_socket_native_fd(client);
        connection->upstream_fd = -1;
        connection->principal_index = SIZE_MAX;
        connection->client_to_upstream.capacity = server->config.buffer_bytes;
        connection->upstream_to_client.capacity = server->config.buffer_bytes;
        connection->client_to_upstream.data = malloc(server->config.buffer_bytes);
        connection->upstream_to_client.data = malloc(server->config.buffer_bytes);
        connection->started_mono_ms = monotonic_now();
        connection->started_unix_ms = wall_now();
        connection->last_activity_ms = connection->started_mono_ms;
        connection->handshake_deadline_ms = add_saturating(
            connection->started_mono_ms, server->config.handshake_timeout_ms);
        (void)pthread_mutex_lock(&server->lifecycle_lock);
        (void)snprintf(connection->policy_digest_hex,
            sizeof(connection->policy_digest_hex), "%s", server->policy->digest_hex);
        connection->policy_generation = atomic_load(&server->policy_generation);
        (void)pthread_mutex_unlock(&server->lifecycle_lock);
        ++server->connection_count;
        (void)atomic_fetch_add(&server->metric_active, 1u);
        if (server->config.tls_provider) {
            char *tls_error = NULL;
            maelys_egress_result_t tls_result =
                server->config.tls_provider->ops.session_create(
                    server->config.tls_provider->context, MAELYS_EGRESS_TLS_SERVER,
                    connection->client_fd, NULL, &connection->tls_session, &tls_error);
            maelys_egress_error_free(tls_error);
            if (tls_result == MAELYS_EGRESS_OK) {
                if (!egress_relay_advance_tls_handshake(server, connection)) {
                    connection->result = MAELYS_EGRESS_ERR_CRYPTO;
                }
            } else {
                connection->result = tls_result;
            }
        }
        if (!connection->client_to_upstream.data || !connection->upstream_to_client.data ||
            (server->config.tls_provider && !connection->tls_session) ||
            connection->result != MAELYS_EGRESS_OK ||
            !egress_connection_update_watches(server, slot)) {
            if (connection->result == MAELYS_EGRESS_OK) {
                connection->result = MAELYS_EGRESS_ERR_MEMORY;
            }
            egress_connection_close(server, slot);
            continue;
        }
    }
}
