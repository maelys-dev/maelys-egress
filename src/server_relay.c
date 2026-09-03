#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "src/server_internal.h"

#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>

/* Data plane: readiness-driven reads and writes on both sides, TLS provider
 * steps, half-close propagation and the per-event connection dispatcher. */

static int read_into(
    maelys_sys_socket_t *socket_handle, egress_buffer_t *buffer, uint64_t *counter,
    int *eof, size_t maximum, int *made_progress) {
    if (made_progress) *made_progress = 0;
    if (!socket_handle || !buffer || !counter || !eof) return 0;
    size_t available = buffer_available(buffer);
    if (available > maximum) available = maximum;
    if (!available) return 1;
    size_t received = 0u;
    maelys_sys_result_t result = maelys_sys_socket_receive(
        socket_handle, buffer->data + buffer->offset + buffer->length,
        available, &received);
    if (result == MAELYS_SYS_OK) {
        buffer->length += received;
        *counter += (uint64_t)received;
        if (made_progress) *made_progress = 1;
        return 1;
    }
    /* System reports EOF and a reset peer alike as ERR_CLOSED: both end the
     * stream and propagate as a half-close. */
    if (result == MAELYS_SYS_ERR_CLOSED) {
        *eof = 1;
        if (made_progress) *made_progress = 1;
        return 1;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static unsigned tls_interest(maelys_egress_tls_step_t step) {
    return step == MAELYS_EGRESS_TLS_WANT_READ ? MAELYS_SYS_INTEREST_READ :
        step == MAELYS_EGRESS_TLS_WANT_WRITE ? MAELYS_SYS_INTEREST_WRITE : 0u;
}

int egress_relay_advance_tls_handshake(
    maelys_egress_server_t *server, egress_connection_t *connection) {
    maelys_egress_tls_provider_t *provider = server->config.tls_provider;
    maelys_egress_tls_step_t step = provider->ops.handshake(
        provider->context, connection->tls_session);
    connection->tls_handshake_interest = tls_interest(step);
    if (step == MAELYS_EGRESS_TLS_COMPLETE) {
        connection->state = CONNECTION_HANDSHAKE;
        connection->tls_handshake_interest = 0u;
        return 1;
    }
    if (step != MAELYS_EGRESS_TLS_WANT_READ && step != MAELYS_EGRESS_TLS_WANT_WRITE) {
        connection->result = MAELYS_EGRESS_ERR_CRYPTO;
    }
    return step == MAELYS_EGRESS_TLS_WANT_READ || step == MAELYS_EGRESS_TLS_WANT_WRITE;
}

static int read_from_client(
    maelys_egress_server_t *server, egress_connection_t *connection,
    int *made_progress) {
    maelys_egress_quota_scope_t limiting_scope = MAELYS_EGRESS_QUOTA_NONE;
    uint64_t allowance = connection->state == CONNECTION_HANDSHAKE ||
        connection->state == CONNECTION_TLS_HANDSHAKE
        ? UINT64_MAX : egress_quota_allowance(server, connection, &limiting_scope);
    if (!allowance) {
        egress_quota_deny(server, connection, limiting_scope);
        connection->quota_exhausted = 1;
        return 1;
    }
    size_t maximum = allowance > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)allowance;
    uint64_t before = connection->bytes_from_client;
    if (!connection->tls_session) {
        int ok = read_into(connection->client_socket, &connection->client_to_upstream,
            &connection->bytes_from_client, &connection->client_eof,
            maximum, made_progress);
        if (ok && connection->state != CONNECTION_HANDSHAKE &&
            connection->state != CONNECTION_TLS_HANDSHAKE)
            egress_quota_charge(server, connection,
                connection->bytes_from_client - before);
        return ok;
    }
    if (made_progress) *made_progress = 0;
    size_t available = buffer_available(&connection->client_to_upstream);
    if (available > maximum) available = maximum;
    if (!available) return 1;
    maelys_egress_tls_provider_t *provider = server->config.tls_provider;
    size_t received = 0u;
    maelys_egress_tls_step_t step = provider->ops.read(
        provider->context, connection->tls_session,
        connection->client_to_upstream.data + connection->client_to_upstream.offset +
            connection->client_to_upstream.length,
        available, &received);
    connection->tls_read_interest = tls_interest(step);
    if (step == MAELYS_EGRESS_TLS_COMPLETE && received) {
        connection->client_to_upstream.length += received;
        connection->bytes_from_client += (uint64_t)received;
        if (connection->state != CONNECTION_HANDSHAKE &&
            connection->state != CONNECTION_TLS_HANDSHAKE)
            egress_quota_charge(server, connection, (uint64_t)received);
        connection->tls_read_interest = 0u;
        if (made_progress) *made_progress = 1;
        return 1;
    }
    if (step == MAELYS_EGRESS_TLS_CLOSED) {
        connection->client_eof = 1;
        connection->tls_read_interest = 0u;
        if (made_progress) *made_progress = 1;
        return 1;
    }
    if (step == MAELYS_EGRESS_TLS_FAILED) {
        connection->result = MAELYS_EGRESS_ERR_CRYPTO;
    }
    return step == MAELYS_EGRESS_TLS_WANT_READ || step == MAELYS_EGRESS_TLS_WANT_WRITE;
}

static int write_from(
    maelys_sys_socket_t *socket_handle, egress_buffer_t *buffer, uint64_t *counter,
    size_t maximum, int *made_progress) {
    if (made_progress) *made_progress = 0;
    if (!buffer->length) return 1;
    size_t written = 0u;
    size_t requested = buffer->length < maximum ? buffer->length : maximum;
    if (!requested || !socket_handle) return 0;
    maelys_sys_result_t result = maelys_sys_socket_send(
        socket_handle, buffer->data + buffer->offset, requested, &written);
    if (result == MAELYS_SYS_OK) {
        buffer_consume(buffer, written);
        *counter += (uint64_t)written;
        if (made_progress && written) *made_progress = 1;
        return 1;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static int write_to_client(
    maelys_egress_server_t *server, egress_connection_t *connection,
    int *made_progress) {
    uint64_t exempt = connection->quota_client_exempt_bytes;
    maelys_egress_quota_scope_t limiting_scope = MAELYS_EGRESS_QUOTA_NONE;
    uint64_t allowance = exempt ? exempt : egress_quota_allowance(
        server, connection, &limiting_scope);
    if (!allowance) {
        egress_quota_deny(server, connection, limiting_scope);
        connection->quota_exhausted = 1;
        return 1;
    }
    size_t maximum = allowance > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)allowance;
    uint64_t before = connection->bytes_to_client;
    if (!connection->tls_session) {
        int ok = write_from(connection->client_socket,
            &connection->upstream_to_client, &connection->bytes_to_client,
            maximum, made_progress);
        uint64_t written = connection->bytes_to_client - before;
        if (ok && written) {
            if (connection->quota_client_exempt_bytes) {
                connection->quota_client_exempt_bytes -= written;
            } else {
                egress_quota_charge(server, connection, written);
            }
        }
        return ok;
    }
    if (made_progress) *made_progress = 0;
    if (!connection->upstream_to_client.length) return 1;
    maelys_egress_tls_provider_t *provider = server->config.tls_provider;
    size_t written = 0u;
    maelys_egress_tls_step_t step = provider->ops.write(
        provider->context, connection->tls_session,
        connection->upstream_to_client.data + connection->upstream_to_client.offset,
        connection->upstream_to_client.length < maximum
            ? connection->upstream_to_client.length : maximum, &written);
    connection->tls_write_interest = tls_interest(step);
    if (step == MAELYS_EGRESS_TLS_COMPLETE && written) {
        buffer_consume(&connection->upstream_to_client, written);
        connection->bytes_to_client += (uint64_t)written;
        if (connection->quota_client_exempt_bytes) {
            connection->quota_client_exempt_bytes -= (uint64_t)written;
        } else {
            egress_quota_charge(server, connection, (uint64_t)written);
        }
        connection->tls_write_interest = 0u;
        if (made_progress) *made_progress = 1;
        return 1;
    }
    if (step == MAELYS_EGRESS_TLS_FAILED) {
        connection->result = MAELYS_EGRESS_ERR_CRYPTO;
    }
    return step == MAELYS_EGRESS_TLS_WANT_READ || step == MAELYS_EGRESS_TLS_WANT_WRITE;
}

static int apply_half_closes(
    maelys_egress_server_t *server, egress_connection_t *connection) {
    if (connection->client_eof && !connection->client_to_upstream.length &&
        connection->upstream_socket && !connection->upstream_write_shutdown) {
        (void)maelys_sys_socket_shutdown(connection->upstream_socket, SHUT_WR);
        connection->upstream_write_shutdown = 1;
    }
    if (connection->upstream_eof && !connection->upstream_to_client.length &&
        connection->client_socket && !connection->client_write_shutdown) {
        if (!connection->tls_session) {
            (void)maelys_sys_socket_shutdown(connection->client_socket, SHUT_WR);
            connection->client_write_shutdown = 1;
        } else {
            maelys_egress_tls_provider_t *provider = server->config.tls_provider;
            maelys_egress_tls_step_t step = provider->ops.shutdown(
                provider->context, connection->tls_session);
            connection->tls_shutdown_interest = tls_interest(step);
            if (step == MAELYS_EGRESS_TLS_COMPLETE || step == MAELYS_EGRESS_TLS_CLOSED) {
                connection->client_write_shutdown = 1;
                connection->tls_shutdown_interest = 0u;
            } else if (step != MAELYS_EGRESS_TLS_WANT_READ &&
                       step != MAELYS_EGRESS_TLS_WANT_WRITE) {
                connection->result = MAELYS_EGRESS_ERR_CRYPTO;
                return 0;
            }
        }
    }
    return 1;
}

static int connection_complete(const egress_connection_t *connection) {
    if (connection->close_after_flush) return connection->upstream_to_client.length == 0u;
    return connection->state == CONNECTION_RELAY && connection->client_eof &&
        connection->upstream_eof && !connection->client_to_upstream.length &&
        !connection->upstream_to_client.length;
}

void egress_relay_dispatch(
    maelys_egress_server_t *server,
    size_t slot,
    unsigned int side,
    unsigned flags) {
    egress_connection_t *connection = &server->connections[slot];
    int ok = 1;
    int progress = 0;
    if (side == TOKEN_SIDE_CLIENT) {
        if (connection->state == CONNECTION_TLS_HANDSHAKE &&
            (flags & connection->tls_handshake_interest)) {
            ok = egress_relay_advance_tls_handshake(server, connection);
        } else if ((flags & (connection->tls_session && connection->tls_read_interest ?
                    connection->tls_read_interest : MAELYS_SYS_INTEREST_READ)) &&
                   !connection->close_after_flush) {
            ok = read_from_client(server, connection, &progress);
            if (ok && progress) connection->last_activity_ms = monotonic_now();
            if (ok && connection->state == CONNECTION_HANDSHAKE) {
                ok = egress_connection_process_handshake(server, slot);
                if (connection->state == CONNECTION_UNUSED) return;
            } else if (ok && connection->state == CONNECTION_IDENTITY) {
                ok = egress_connection_enforce_tunnel_identity(server, slot);
                if (connection->state == CONNECTION_UNUSED) return;
            }
        }
        if (ok && connection->state != CONNECTION_TLS_HANDSHAKE &&
            connection->upstream_to_client.length &&
            (flags & (connection->tls_session && connection->tls_write_interest ?
                connection->tls_write_interest : MAELYS_SYS_INTEREST_WRITE))) {
            ok = write_to_client(server, connection, &progress);
            if (ok && progress) connection->last_activity_ms = monotonic_now();
        }
        if ((flags & (MAELYS_SYS_EVENT_ERROR | MAELYS_SYS_EVENT_HUP)) &&
            !(flags & MAELYS_SYS_EVENT_READ)) connection->client_eof = 1;
    } else if (side == TOKEN_SIDE_UPSTREAM) {
        if (connection->state == CONNECTION_CONNECTING &&
            (flags & (MAELYS_SYS_EVENT_WRITE | MAELYS_SYS_EVENT_ERROR |
                      MAELYS_SYS_EVENT_HUP))) {
            if (maelys_sys_socket_connect_complete(connection->upstream_socket) ==
                MAELYS_SYS_OK) {
                egress_connection_connected(server, slot);
                if (connection->state == CONNECTION_UNUSED) return;
            } else {
                (void)maelys_sys_loop_unwatch(server->loop, connection->upstream_watch);
                connection->upstream_watch = 0u;
                egress_socket_release(&connection->upstream_socket, &connection->upstream_fd);
                ok = egress_connection_connect_next(server, slot);
            }
        } else if (connection->state == CONNECTION_RELAY) {
            if (flags & MAELYS_SYS_EVENT_READ) {
                uint64_t ignored = 0u;
                ok = read_into(connection->upstream_socket, &connection->upstream_to_client,
                               &ignored, &connection->upstream_eof, SIZE_MAX,
                               &progress);
                if (ok && progress) connection->last_activity_ms = monotonic_now();
            }
            if (ok && (flags & MAELYS_SYS_EVENT_WRITE)) {
                uint64_t ignored = 0u;
                ok = write_from(connection->upstream_socket,
                                &connection->client_to_upstream, &ignored,
                                SIZE_MAX, &progress);
                if (ok && progress) connection->last_activity_ms = monotonic_now();
            }
            if ((flags & (MAELYS_SYS_EVENT_ERROR | MAELYS_SYS_EVENT_HUP)) &&
                !(flags & MAELYS_SYS_EVENT_READ)) connection->upstream_eof = 1;
        }
    }
    if (ok && connection->quota_admitted) {
        maelys_egress_quota_scope_t quota_scope = MAELYS_EGRESS_QUOTA_NONE;
        if (egress_quota_allowance(server, connection, &quota_scope) == 0u &&
            quota_scope != MAELYS_EGRESS_QUOTA_NONE) {
            egress_quota_deny(server, connection, quota_scope);
            connection->quota_exhausted = 1;
        }
    }
    if (ok && connection->quota_exhausted &&
        !connection->client_to_upstream.length) {
        egress_connection_close(server, slot);
        return;
    }
    if (!ok) {
        if (connection->result == MAELYS_EGRESS_OK) {
            connection->result = MAELYS_EGRESS_ERR_IO;
        }
        egress_connection_close(server, slot);
        return;
    }
    if (ok) ok = apply_half_closes(server, connection);
    if (!ok) {
        if (connection->result == MAELYS_EGRESS_OK) {
            connection->result = MAELYS_EGRESS_ERR_IO;
        }
        egress_connection_close(server, slot);
        return;
    }
    if (connection_complete(connection)) {
        egress_connection_close(server, slot);
    } else if (!egress_connection_update_watches(server, slot)) {
        connection->result = MAELYS_EGRESS_ERR_IO;
        egress_connection_close(server, slot);
    }
}
