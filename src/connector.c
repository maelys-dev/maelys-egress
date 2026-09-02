#include "src/internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct maelys_egress_connector {
    atomic_uint references;
    maelys_egress_server_t *server;
    size_t principal_index;
    char principal[EGRESS_MAX_USERNAME + 1u];
    char invocation_id[EGRESS_MAX_INVOCATION_ID + 1u];
};

struct maelys_egress_session {
    int fd;
};

maelys_egress_result_t maelys_egress_server_connector_create(
    maelys_egress_server_t *server,
    const char *username,
    const char *secret,
    maelys_egress_connector_t **out_connector,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_connector) *out_connector = NULL;
    if (!server || !username || !secret || !out_connector) {
        egress_set_error(out_error, "server, credentials and output are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    maelys_egress_connector_t *connector = calloc(1, sizeof(*connector));
    if (!connector) return MAELYS_EGRESS_ERR_MEMORY;
    maelys_egress_result_t result = egress_server_connector_bind(
        server, username, secret, &connector->principal_index,
        connector->principal, connector->invocation_id, out_error);
    if (result != MAELYS_EGRESS_OK) {
        free(connector);
        return result;
    }
    atomic_init(&connector->references, 1u);
    connector->server = server;
    *out_connector = connector;
    return MAELYS_EGRESS_OK;
}

void maelys_egress_connector_retain(maelys_egress_connector_t *connector) {
    if (connector) (void)atomic_fetch_add(&connector->references, 1u);
}

void maelys_egress_connector_release(maelys_egress_connector_t *connector) {
    if (!connector || atomic_fetch_sub(&connector->references, 1u) != 1u) return;
    egress_server_connector_release(connector->server);
    egress_secure_zero(connector, sizeof(*connector));
    free(connector);
}

maelys_egress_result_t maelys_egress_connector_session_open(
    maelys_egress_connector_t *connector,
    const char *canonical_host,
    uint16_t port,
    uint64_t timeout_ms,
    maelys_egress_session_t **out_session,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_session) *out_session = NULL;
    if (!connector || !canonical_host || !canonical_host[0] || !port ||
        !timeout_ms || !out_session) {
        egress_set_error(out_error,
            "connector, canonical destination, finite timeout and output are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    maelys_egress_session_t *session = malloc(sizeof(*session));
    if (!session) return MAELYS_EGRESS_ERR_MEMORY;
    session->fd = -1;
    maelys_egress_result_t result = egress_server_open_stream(
        connector->server, connector->principal_index, connector->principal,
        connector->invocation_id, canonical_host, port, timeout_ms,
        &session->fd, out_error);
    if (result != MAELYS_EGRESS_OK) {
        free(session);
        return result;
    }
    *out_session = session;
    return MAELYS_EGRESS_OK;
}

int maelys_egress_session_fd(const maelys_egress_session_t *session) {
    return session ? session->fd : -1;
}

maelys_egress_result_t maelys_egress_session_take_fd(
    maelys_egress_session_t *session,
    int *out_fd,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_fd) *out_fd = -1;
    if (!session || !out_fd || session->fd < 0) {
        egress_set_error(out_error, "a live session and output fd are required");
        return MAELYS_EGRESS_ERR_STATE;
    }
    *out_fd = session->fd;
    session->fd = -1;
    return MAELYS_EGRESS_OK;
}

void maelys_egress_session_release(maelys_egress_session_t *session) {
    if (!session) return;
    (void)maelys_sys_fd_close(&session->fd);
    free(session);
}
