#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "src/server_internal.h"

#include <stdio.h>

/* Receipt emission at connection close: canonical attestation input,
 * optional asymmetric attestation, receipt sink callback and durable audit. */

static void bytes_to_hex(
    const unsigned char *bytes, size_t length, char *output) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        output[i * 2u] = hex[bytes[i] >> 4u];
        output[i * 2u + 1u] = hex[bytes[i] & 0x0fu];
    }
    output[length * 2u] = '\0';
}

void egress_receipt_emit(
    maelys_egress_server_t *server,
    const egress_connection_t *connection) {
    uint64_t now = monotonic_now();
    maelys_egress_receipt_t receipt = {
        .id = connection->generation,
        .protocol = connection->protocol,
        .port = connection->port,
        .result = connection->result,
        .started_unix_ms = connection->started_unix_ms,
        .duration_ms = now >= connection->started_mono_ms ?
            now - connection->started_mono_ms : 0u,
        .bytes_from_client = connection->bytes_from_client,
        .bytes_to_client = connection->bytes_to_client,
        .quota_scope = connection->quota_scope,
        .quota_connection_max_bytes = connection->quota_bytes,
        .quota_execution_max_bytes = connection->quota_total_bytes,
        .quota_connection_observed_bytes = connection->quota_stream_used,
        .quota_execution_before_bytes = connection->quota_total_before,
        .quota_execution_after_bytes =
            connection->principal_index < server->config.principal_count
                ? server->principal_bytes_total[connection->principal_index] : 0u
    };
    (void)snprintf(receipt.host, sizeof(receipt.host), "%s", connection->host);
    (void)snprintf(receipt.invocation_id, sizeof(receipt.invocation_id), "%s",
                   connection->invocation_id);
    receipt.tls_sni_verified = connection->tls_sni_verified;
    (void)snprintf(receipt.policy_digest_hex, sizeof(receipt.policy_digest_hex),
                   "%s", connection->policy_digest_hex);
    (void)snprintf(receipt.principal, sizeof(receipt.principal), "%s",
                   connection->principal);
    receipt.policy_generation = connection->policy_generation;
    int attestation_ok = 1;
    if (server->config.attestor) {
        maelys_egress_attestor_t *attestor = server->config.attestor;
        char canonical[2048];
        int canonical_length = egress_receipt_canonical(
            &receipt, canonical, sizeof(canonical));
        unsigned char signature[EGRESS_MAX_ATTESTATION_BYTES];
        size_t signature_length = 0u;
        char *attestation_error = NULL;
        maelys_egress_result_t result = canonical_length > 0 ?
            attestor->attest(attestor->context, canonical,
                (size_t)canonical_length, signature,
                attestor->max_signature_bytes, &signature_length,
                &attestation_error) : MAELYS_EGRESS_ERR_MEMORY;
        attestation_ok = result == MAELYS_EGRESS_OK && signature_length > 0u &&
            signature_length <= attestor->max_signature_bytes;
        maelys_egress_error_free(attestation_error);
        if (attestation_ok) {
            (void)snprintf(receipt.attestor, sizeof(receipt.attestor), "%s",
                           attestor->name);
            (void)snprintf(receipt.attestation_key_id,
                sizeof(receipt.attestation_key_id), "%s", attestor->key_id);
            bytes_to_hex(signature, signature_length, receipt.attestation_hex);
        } else {
            (void)atomic_fetch_add(&server->metric_audit_failures, 1u);
        }
        egress_secure_zero(signature, sizeof(signature));
    }
    if (server->config.receipt_sink) {
        server->config.receipt_sink(server->config.receipt_context, &receipt);
    }
    (void)atomic_fetch_add(&server->metric_receipts, 1u);
    if (server->config.audit && attestation_ok &&
        !egress_audit_append(server->config.audit, &receipt)) {
        (void)atomic_fetch_add(&server->metric_audit_failures, 1u);
    }
}
