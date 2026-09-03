#ifndef MAELYS_EGRESS_SERVER_INTERNAL_H
#define MAELYS_EGRESS_SERVER_INTERNAL_H

/*
 * Private contract shared by the server implementation files. Nothing here
 * is part of the public ABI. One owner thread runs the reactor and every
 * function below that takes a live slot is called on that thread unless its
 * comment says otherwise.
 *
 *   server.c             lifecycle, reactor loop, deadlines, public API
 *   server_listener.c    TCP/AF_UNIX listeners, private connector TCP pair
 *   server_connection.c  admission, handshake, policy match, connect, close
 *   server_relay.c       readiness-driven relay, TLS steps, half-close
 *   server_quota.c       per-stream and per-principal byte accounting
 *   server_receipt.c     receipt emission, attestation, durable audit
 *   server_connector.c   native connector open commands
 *   server_admin.c       loopback /healthz and /metrics responder
 */

#include "src/internal.h"

#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#define TOKEN_LISTENER UINT64_C(1)
#define TOKEN_ADMIN_LISTENER UINT64_C(4)
#define TOKEN_ADMIN_FLAG (UINT64_C(1) << 63u)
#define TOKEN_SIDE_CLIENT UINT64_C(2)
#define TOKEN_SIDE_UPSTREAM UINT64_C(3)
#define TOKEN_SLOT_SHIFT 2u
#define TOKEN_GENERATION_SHIFT 16u
#define EGRESS_MAX_ADMIN_CONNECTIONS 16u
#define EGRESS_ADMIN_BUFFER 4096u

typedef struct egress_buffer {
    unsigned char *data;
    size_t capacity;
    size_t offset;
    size_t length;
} egress_buffer_t;

typedef enum connection_state {
    CONNECTION_UNUSED = 0,
    CONNECTION_TLS_HANDSHAKE,
    CONNECTION_HANDSHAKE,
    CONNECTION_CONNECTING,
    CONNECTION_IDENTITY,
    CONNECTION_RELAY
} connection_state_t;

typedef struct egress_open_command egress_open_command_t;

typedef struct egress_connection {
    connection_state_t state;
    uint64_t generation;
    /* Sockets are owned System handles; the int views are borrowed native
     * descriptors for the reactor and TLS providers, -1 when the handle is NULL. */
    maelys_sys_socket_t *client_socket;
    maelys_sys_socket_t *upstream_socket;
    int client_fd;
    int upstream_fd;
    maelys_sys_watch_t client_watch;
    maelys_sys_watch_t upstream_watch;
    egress_buffer_t client_to_upstream;
    egress_buffer_t upstream_to_client;
    int socks_phase;
    int close_after_flush;
    int client_eof;
    int upstream_eof;
    int client_write_shutdown;
    int upstream_write_shutdown;
    /* Peer half-close seen while READ was not armed: reads resume when the
     * buffer has room and the watch is dropped meanwhile, so a level-triggered
     * HUP never spins and no queued bytes are lost. */
    int client_hup;
    int upstream_hup;
    const egress_destination_t *destination;
    egress_destination_t destination_snapshot;
    char policy_digest_hex[65];
    uint64_t policy_generation;
    size_t address_index;
    maelys_egress_protocol_t protocol;
    char host[EGRESS_MAX_HOST + 1u];
    char authenticated_invocation_id[EGRESS_MAX_INVOCATION_ID + 1u];
    char invocation_id[EGRESS_MAX_INVOCATION_ID + 1u];
    char principal[EGRESS_MAX_USERNAME + 1u];
    size_t principal_index;
    uint64_t quota_bytes;
    uint64_t quota_total_bytes;
    uint64_t quota_stream_used;
    uint64_t quota_total_before;
    uint64_t quota_client_exempt_bytes;
    maelys_egress_quota_scope_t quota_scope;
    int quota_exhausted;
    int quota_admitted;
    uint16_t port;
    uint64_t started_mono_ms;
    uint64_t started_unix_ms;
    uint64_t last_activity_ms;
    uint64_t handshake_deadline_ms;
    uint64_t bytes_from_client;
    uint64_t bytes_to_client;
    maelys_egress_result_t result;
    int tls_sni_verified;
    void *tls_session;
    unsigned tls_handshake_interest;
    unsigned tls_read_interest;
    unsigned tls_write_interest;
    unsigned tls_shutdown_interest;
    egress_open_command_t *open_command;
} egress_connection_t;

struct egress_open_command {
    atomic_uint references;
    atomic_int cancelled;
    maelys_sys_mutex_t *mutex;
    maelys_sys_condition_t *condition;
    egress_open_command_t *next;
    /* Egress relays server_socket; client_fd is the blocking TCP end handed
     * to the embedder, so it stays a bare descriptor. */
    maelys_sys_socket_t *server_socket;
    int client_fd;
    int completed;
    maelys_egress_result_t result;
    size_t principal_index;
    char principal[EGRESS_MAX_USERNAME + 1u];
    char invocation_id[EGRESS_MAX_INVOCATION_ID + 1u];
    char host[EGRESS_MAX_HOST + 1u];
    uint16_t port;
};

typedef struct egress_admin_connection {
    maelys_sys_socket_t *socket;
    int fd;
    maelys_sys_watch_t watch;
    uint64_t generation;
    uint64_t deadline_ms;
    unsigned char request[EGRESS_ADMIN_BUFFER];
    size_t request_length;
    char response[EGRESS_ADMIN_BUFFER];
    size_t response_offset;
    size_t response_length;
} egress_admin_connection_t;

struct maelys_egress_server {
    atomic_uint references;
    maelys_egress_policy_t *policy;
    maelys_egress_config_t config;
    maelys_sys_loop_t *loop;
    maelys_sys_socket_t *listener_socket;
    int listener_fd;
    maelys_sys_watch_t listener_watch;
    uint16_t bound_port;
    maelys_sys_socket_t *admin_listener_socket;
    int admin_listener_fd;
    maelys_sys_watch_t admin_listener_watch;
    uint16_t admin_bound_port;
    dev_t unix_socket_device;
    ino_t unix_socket_inode;
    int unix_socket_created;
    egress_connection_t *connections;
    size_t connection_count;
    atomic_int started;
    atomic_int running;
    atomic_int stopping;
    pthread_mutex_t lifecycle_lock;
    pthread_mutex_t command_lock;
    egress_open_command_t *command_head;
    egress_open_command_t *command_tail;
    atomic_int finalized;
    uint64_t next_id;
    atomic_uint_fast64_t policy_generation;
    size_t principal_active[EGRESS_MAX_PRINCIPALS];
    uint64_t principal_bytes_total[EGRESS_MAX_PRINCIPALS];
    atomic_uint_fast64_t metric_accepted;
    atomic_uint_fast64_t metric_active;
    atomic_uint_fast64_t metric_admitted;
    atomic_uint_fast64_t metric_denied;
    atomic_uint_fast64_t metric_auth_failures;
    atomic_uint_fast64_t metric_quota_denials;
    atomic_uint_fast64_t metric_bytes_from_clients;
    atomic_uint_fast64_t metric_bytes_to_clients;
    atomic_uint_fast64_t metric_receipts;
    atomic_uint_fast64_t metric_reload_failures;
    atomic_uint_fast64_t metric_audit_failures;
    egress_admin_connection_t admin_connections[EGRESS_MAX_ADMIN_CONNECTIONS];
    uint64_t next_admin_generation;
};
/* Clock and arithmetic helpers. */

static inline uint64_t monotonic_now(void) {
    uint64_t now = 0u;
    (void)maelys_sys_monotonic_ms(&now);
    return now;
}

static inline uint64_t wall_now(void) {
    uint64_t now = 0u;
    (void)maelys_sys_wall_ms(&now);
    return now;
}

static inline uint64_t add_saturating(uint64_t value, uint64_t amount) {
    return UINT64_MAX - value < amount ? UINT64_MAX : value + amount;
}

static inline void buffer_compact(egress_buffer_t *buffer) {
    if (buffer->offset && buffer->length) {
        memmove(buffer->data, buffer->data + buffer->offset, buffer->length);
    }
    buffer->offset = 0u;
}

static inline size_t buffer_available(egress_buffer_t *buffer) {
    if (buffer->offset + buffer->length == buffer->capacity && buffer->offset) {
        buffer_compact(buffer);
    }
    return buffer->capacity - buffer->offset - buffer->length;
}

static inline int buffer_append(egress_buffer_t *buffer, const void *bytes, size_t length) {
    if (buffer_available(buffer) < length) return 0;
    memcpy(buffer->data + buffer->offset + buffer->length, bytes, length);
    buffer->length += length;
    return 1;
}

static inline void buffer_consume(egress_buffer_t *buffer, size_t length) {
    if (length >= buffer->length) {
        buffer->offset = 0u;
        buffer->length = 0u;
        return;
    }
    buffer->offset += length;
    buffer->length -= length;
}

static inline uint64_t connection_token(size_t slot, uint64_t generation, uint64_t side) {
    return (generation << TOKEN_GENERATION_SHIFT) |
        ((uint64_t)slot << TOKEN_SLOT_SHIFT) | side;
}

static inline void decode_token(uint64_t token, size_t *slot, uint64_t *generation,
                         unsigned int *side) {
    *side = (unsigned int)(token & UINT64_C(3));
    *slot = (size_t)((token >> TOKEN_SLOT_SHIFT) & UINT64_C(0x3fff));
    *generation = token >> TOKEN_GENERATION_SHIFT;
}
/* server.c */
void egress_server_control_release(maelys_egress_server_t *server);

/* server_listener.c */
int egress_listener_is_loopback_host(const char *host);
maelys_sys_socket_t *egress_listener_create_tcp(
    const maelys_egress_config_t *config, uint16_t *out_port);
void egress_listener_unlink_unix_identity(const char *path, dev_t device, ino_t inode);
maelys_sys_socket_t *egress_listener_create_unix(
    const maelys_egress_config_t *config, dev_t *out_device, ino_t *out_inode);
int egress_listener_unix_peer_allowed(const maelys_egress_server_t *server, int client);
int egress_listener_create_private_tcp_pair(
    maelys_sys_socket_t **out_server, int *out_client);
/* Releases the System handle and resets its borrowed descriptor view. */
void egress_socket_release(maelys_sys_socket_t **socket_handle, int *fd_view);

/* server_quota.c */
uint64_t egress_quota_allowance(
    const maelys_egress_server_t *server,
    const egress_connection_t *connection,
    maelys_egress_quota_scope_t *out_scope);
void egress_quota_charge(
    maelys_egress_server_t *server, egress_connection_t *connection, uint64_t amount);
void egress_quota_deny(
    maelys_egress_server_t *server, egress_connection_t *connection,
    maelys_egress_quota_scope_t scope);

/* server_receipt.c */
void egress_receipt_emit(
    maelys_egress_server_t *server, const egress_connection_t *connection);

/* server_connection.c */
void egress_connection_close(maelys_egress_server_t *server, size_t slot);
void egress_connection_fail(
    maelys_egress_server_t *server, size_t slot, maelys_egress_result_t result,
    int authentication_failure);
int egress_connection_update_watches(maelys_egress_server_t *server, size_t slot);
int egress_connection_enforce_tunnel_identity(maelys_egress_server_t *server, size_t slot);
void egress_connection_connected(maelys_egress_server_t *server, size_t slot);
int egress_connection_connect_next(maelys_egress_server_t *server, size_t slot);
int egress_connection_begin_request(
    maelys_egress_server_t *server, size_t slot, const egress_proxy_request_t *request);
int egress_connection_process_handshake(maelys_egress_server_t *server, size_t slot);
void egress_connection_accept_all(maelys_egress_server_t *server);

/* server_relay.c */
int egress_relay_advance_tls_handshake(
    maelys_egress_server_t *server, egress_connection_t *connection);
void egress_relay_dispatch(
    maelys_egress_server_t *server, size_t slot, unsigned int side, unsigned flags);

/* server_connector.c. Open commands are queued from embedder threads and
 * completed on the owner thread; the bind/open entry points are declared in
 * src/internal.h for src/connector.c. */
void egress_open_command_complete(
    egress_open_command_t *command, maelys_egress_result_t result);
void egress_open_commands_process(maelys_egress_server_t *server);
void egress_open_commands_cancel(maelys_egress_server_t *server);
void egress_open_commands_reject(
    maelys_egress_server_t *server, maelys_egress_result_t result);

/* server_admin.c */
void egress_admin_close(
    maelys_egress_server_t *server, egress_admin_connection_t *connection);
void egress_admin_dispatch(maelys_egress_server_t *server, size_t slot, unsigned flags);
void egress_admin_accept_all(maelys_egress_server_t *server);

#endif
