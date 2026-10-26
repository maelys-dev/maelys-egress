#include "src/internal.h"

#include <stdio.h>

/* The one canonical receipt encoding. It is the exact byte string that the
 * optional attestor signs and the core of every durable audit record, so
 * the two consumers cannot drift apart. The field order and spellings are a
 * public evidence format.
 *
 * Both consumers treat these bytes as opaque: an attestation and a journal
 * record verify over the bytes stored, never over a receipt rebuilt from
 * fields, so what was stored verifies for ever. A field is therefore only
 * ever appended at the end, and a reader that parses key=value tolerates a
 * trailing key it does not know. Moving, removing or respelling a field is
 * a break of the format, named in the changelog. Every string here keeps
 * out of '|', '=', '"' and '\', which lets a reader cut this core out of a
 * journal record. `address`, appended in 0.34.0, is the upstream address
 * the connection was established to, empty when none was. */

int egress_receipt_canonical(
    const maelys_egress_receipt_t *receipt, char *buffer, size_t capacity) {
    if (!receipt || !buffer || capacity == 0u) return -1;
    int needed = snprintf(buffer, capacity,
        "id=%llu|principal=%s|invocation=%s|protocol=%d|host=%s|port=%u|"
        "result=%d|started=%llu|duration=%llu|from=%llu|to=%llu|policy=%s|generation=%llu|sni=%d|"
        "quota-scope=%d|quota-connection-max=%llu|quota-execution-max=%llu|"
        "quota-connection-observed=%llu|quota-execution-before=%llu|quota-execution-after=%llu|"
        "address=%s",
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
        (unsigned long long)receipt->quota_execution_after_bytes,
        receipt->upstream_address);
    if (needed < 0 || (size_t)needed >= capacity) return -1;
    return needed;
}
