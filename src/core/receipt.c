#include "src/internal.h"

#include <stdio.h>

/* The one canonical receipt encoding. It is the exact byte string that the
 * optional attestor signs and the core of every durable audit record, so
 * the two consumers cannot drift apart. The field order and spellings are a
 * public evidence format: changing them invalidates existing signatures and
 * journals and requires a new audit record version. */

int egress_receipt_canonical(
    const maelys_egress_receipt_t *receipt, char *buffer, size_t capacity) {
    if (!receipt || !buffer || capacity == 0u) return -1;
    int needed = snprintf(buffer, capacity,
        "id=%llu|principal=%s|invocation=%s|protocol=%d|host=%s|port=%u|"
        "result=%d|started=%llu|duration=%llu|from=%llu|to=%llu|policy=%s|generation=%llu|sni=%d|"
        "quota-scope=%d|quota-connection-max=%llu|quota-execution-max=%llu|"
        "quota-connection-observed=%llu|quota-execution-before=%llu|quota-execution-after=%llu",
        (unsigned long long)receipt->id, receipt->principal,
        receipt->invocation_id, (int)receipt->protocol, receipt->host,
        (unsigned int)receipt->port, (int)receipt->result,
        (unsigned long long)receipt->started_unix_ms,
        (unsigned long long)receipt->duration_ms,
        (unsigned long long)receipt->bytes_from_client,
        (unsigned long long)receipt->bytes_to_client,
        receipt->policy_digest_hex,
        (unsigned long long)receipt->policy_generation,
        receipt->tls_sni_verified, (int)receipt->quota_scope,
        (unsigned long long)receipt->quota_connection_max_bytes,
        (unsigned long long)receipt->quota_execution_max_bytes,
        (unsigned long long)receipt->quota_connection_observed_bytes,
        (unsigned long long)receipt->quota_execution_before_bytes,
        (unsigned long long)receipt->quota_execution_after_bytes);
    if (needed < 0 || (size_t)needed >= capacity) return -1;
    return needed;
}
