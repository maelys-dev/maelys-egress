#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "src/server_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

/* Separate loopback operations listener: bounded /healthz and /metrics
 * responder with its own slots, generations and deadlines. */

static uint64_t admin_token(size_t slot, uint64_t generation) {
    return TOKEN_ADMIN_FLAG | (generation << TOKEN_GENERATION_SHIFT) |
        ((uint64_t)slot << TOKEN_SLOT_SHIFT) | TOKEN_SIDE_CLIENT;
}

void egress_admin_close(
    maelys_egress_server_t *server, egress_admin_connection_t *connection) {
    if (connection->watch) {
        (void)maelys_sys_loop_unwatch(server->loop, connection->watch);
    }
    egress_socket_release(&connection->socket, &connection->fd);
    uint64_t generation = connection->generation;
    memset(connection, 0, sizeof(*connection));
    connection->fd = -1;
    connection->generation = generation;
}

static int admin_watch(
    maelys_egress_server_t *server, size_t slot, unsigned interests) {
    egress_admin_connection_t *connection = &server->admin_connections[slot];
    uint64_t token = admin_token(slot, connection->generation);
    if (connection->watch) {
        return maelys_sys_loop_modify(
            server->loop, connection->watch, interests) == MAELYS_SYS_OK;
    }
    return maelys_sys_loop_watch_fd(
        server->loop, connection->fd, interests, token,
        &connection->watch) == MAELYS_SYS_OK;
}

static void prepare_admin_response(
    maelys_egress_server_t *server, egress_admin_connection_t *connection) {
    connection->request[connection->request_length] = '\0';
    const char *end = strstr((const char *)connection->request, "\r\n\r\n");
    if (!end) return;
    int health = strncmp((const char *)connection->request,
        "GET /healthz HTTP/1.1\r\n", 23u) == 0;
    int metrics = strncmp((const char *)connection->request,
        "GET /metrics HTTP/1.1\r\n", 23u) == 0;
    char body[3072];
    int body_length;
    const char *status;
    const char *content_type;
    if (health) {
        int healthy = (!server->config.audit ||
            maelys_egress_audit_healthy(server->config.audit)) &&
            atomic_load(&server->metric_audit_failures) == 0u;
        status = healthy ? "200 OK" : "503 Service Unavailable";
        content_type = "application/json";
        body_length = snprintf(body, sizeof(body),
            "{\"status\":\"%s\",\"policy_generation\":%llu,"
            "\"audit_healthy\":%s}\n",
            healthy ? "ok" : "degraded",
            (unsigned long long)atomic_load(&server->policy_generation),
            healthy ? "true" : "false");
    } else if (metrics) {
        status = "200 OK";
        content_type = "text/plain; version=0.0.4";
        body_length = snprintf(body, sizeof(body),
            "# TYPE maelys_egress_connections_total counter\n"
            "maelys_egress_connections_total %llu\n"
            "# TYPE maelys_egress_connections_active gauge\n"
            "maelys_egress_connections_active %llu\n"
            "maelys_egress_admissions_total %llu\n"
            "maelys_egress_denials_total %llu\n"
            "maelys_egress_auth_failures_total %llu\n"
            "maelys_egress_quota_denials_total %llu\n"
            "maelys_egress_bytes_from_clients_total %llu\n"
            "maelys_egress_bytes_to_clients_total %llu\n"
            "maelys_egress_receipts_total %llu\n"
            "maelys_egress_policy_generation %llu\n"
            "maelys_egress_policy_reload_failures_total %llu\n"
            "maelys_egress_audit_failures_total %llu\n",
            (unsigned long long)atomic_load(&server->metric_accepted),
            (unsigned long long)atomic_load(&server->metric_active),
            (unsigned long long)atomic_load(&server->metric_admitted),
            (unsigned long long)atomic_load(&server->metric_denied),
            (unsigned long long)atomic_load(&server->metric_auth_failures),
            (unsigned long long)atomic_load(&server->metric_quota_denials),
            (unsigned long long)atomic_load(&server->metric_bytes_from_clients),
            (unsigned long long)atomic_load(&server->metric_bytes_to_clients),
            (unsigned long long)atomic_load(&server->metric_receipts),
            (unsigned long long)atomic_load(&server->policy_generation),
            (unsigned long long)atomic_load(&server->metric_reload_failures),
            (unsigned long long)atomic_load(&server->metric_audit_failures));
    } else {
        status = "404 Not Found";
        content_type = "text/plain";
        body_length = snprintf(body, sizeof(body), "not found\n");
    }
    if (body_length < 0 || (size_t)body_length >= sizeof(body)) {
        egress_admin_close(server, connection);
        return;
    }
    int response_length = snprintf(connection->response,
        sizeof(connection->response),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n\r\n%s",
        status, content_type, (unsigned int)body_length, body);
    if (response_length < 0 || (size_t)response_length >= sizeof(connection->response)) {
        egress_admin_close(server, connection);
        return;
    }
    connection->response_length = (size_t)response_length;
}

void egress_admin_dispatch(
    maelys_egress_server_t *server, size_t slot, unsigned flags) {
    egress_admin_connection_t *connection = &server->admin_connections[slot];
    if (!connection->response_length && (flags & MAELYS_SYS_EVENT_READ)) {
        size_t available = sizeof(connection->request) - 1u - connection->request_length;
        size_t amount = 0u;
        maelys_sys_result_t received = maelys_sys_socket_receive(connection->socket,
            connection->request + connection->request_length, available, &amount);
        if (received == MAELYS_SYS_OK) {
            connection->request_length += amount;
            prepare_admin_response(server, connection);
        } else if (received == MAELYS_SYS_ERR_CLOSED ||
                   (errno != EAGAIN && errno != EWOULDBLOCK)) {
            egress_admin_close(server, connection);
            return;
        }
        if (!connection->response_length && !available) {
            egress_admin_close(server, connection);
            return;
        }
    }
    if (connection->response_length && (flags & MAELYS_SYS_EVENT_WRITE)) {
        size_t written = 0u;
        maelys_sys_result_t result = maelys_sys_socket_send(
            connection->socket, connection->response + connection->response_offset,
            connection->response_length - connection->response_offset, &written);
        if (result != MAELYS_SYS_OK && errno != EAGAIN && errno != EWOULDBLOCK) {
            egress_admin_close(server, connection);
            return;
        }
        connection->response_offset += written;
        if (connection->response_offset == connection->response_length) {
            egress_admin_close(server, connection);
            return;
        }
    }
    if (flags & (MAELYS_SYS_EVENT_ERROR | MAELYS_SYS_EVENT_HUP)) {
        egress_admin_close(server, connection);
        return;
    }
    (void)admin_watch(server, slot,
        connection->response_length ? MAELYS_SYS_INTEREST_WRITE : MAELYS_SYS_INTEREST_READ);
}

void egress_admin_accept_all(maelys_egress_server_t *server) {
    for (;;) {
        maelys_sys_socket_t *client = NULL;
        if (maelys_sys_socket_accept(server->admin_listener_socket, NULL, NULL, &client) !=
            MAELYS_SYS_OK) return;
        size_t slot = 0u;
        while (slot < EGRESS_MAX_ADMIN_CONNECTIONS &&
               server->admin_connections[slot].fd >= 0) ++slot;
        if (slot == EGRESS_MAX_ADMIN_CONNECTIONS) {
            (void)maelys_sys_socket_release(&client); continue;
        }
        egress_admin_connection_t *connection = &server->admin_connections[slot];
        memset(connection, 0, sizeof(*connection));
        connection->socket = client;
        connection->fd = maelys_sys_socket_native_fd(client);
        connection->generation = ++server->next_admin_generation;
        connection->deadline_ms = add_saturating(monotonic_now(), 5000u);
        if (!admin_watch(server, slot, MAELYS_SYS_INTEREST_READ)) {
            egress_admin_close(server, connection);
        }
    }
}
