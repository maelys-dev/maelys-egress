#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "src/server_internal.h"

#include <stdint.h>

/* Per-connection and per-principal byte quotas. Every payload read or write
 * asks for an allowance first and charges exactly what the kernel moved, so
 * concurrent and sequential streams of one principal never overshoot. */

static uint64_t quota_remaining(uint64_t maximum, uint64_t used) {
    if (!maximum) return UINT64_MAX;
    return used >= maximum ? 0u : maximum - used;
}

uint64_t egress_quota_allowance(
    const maelys_egress_server_t *server,
    const egress_connection_t *connection,
    maelys_egress_quota_scope_t *out_scope) {
    if (out_scope) *out_scope = MAELYS_EGRESS_QUOTA_NONE;
    if (!connection->quota_admitted ||
        connection->principal_index >= server->config.principal_count)
        return UINT64_MAX;
    uint64_t stream = quota_remaining(
        connection->quota_bytes, connection->quota_stream_used);
    uint64_t total = quota_remaining(
        connection->quota_total_bytes,
        server->principal_bytes_total[connection->principal_index]);
    if (connection->quota_total_bytes && total <= stream) {
        if (out_scope) *out_scope = MAELYS_EGRESS_QUOTA_EXECUTION_BYTES;
        return total;
    }
    if (connection->quota_bytes) {
        if (out_scope) *out_scope = MAELYS_EGRESS_QUOTA_CONNECTION_BYTES;
        return stream;
    }
    return UINT64_MAX;
}

void egress_quota_charge(
    maelys_egress_server_t *server,
    egress_connection_t *connection,
    uint64_t amount) {
    if (!amount || !connection->quota_admitted ||
        connection->principal_index >= server->config.principal_count) return;
    connection->quota_stream_used = add_saturating(
        connection->quota_stream_used, amount);
    server->principal_bytes_total[connection->principal_index] = add_saturating(
        server->principal_bytes_total[connection->principal_index], amount);
}

void egress_quota_deny(
    maelys_egress_server_t *server,
    egress_connection_t *connection,
    maelys_egress_quota_scope_t scope) {
    if (connection->quota_scope == MAELYS_EGRESS_QUOTA_NONE) {
        connection->quota_scope = scope;
        (void)atomic_fetch_add(&server->metric_quota_denials, 1u);
    }
    connection->result = MAELYS_EGRESS_ERR_DENIED;
}
