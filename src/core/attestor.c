#include "src/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int safe_identity(const char *value, size_t maximum) {
    if (!value || !value[0] || strlen(value) >= maximum) return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        int allowed = (*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == ':' || *p == '/';
        if (!allowed) return 0;
    }
    return 1;
}

maelys_egress_result_t maelys_egress_attestor_create(
    const char *name, const char *key_id, size_t max_signature_bytes,
    maelys_egress_attest_fn attest, void *context,
    void (*release_context)(void *context),
    maelys_egress_attestor_t **out_attestor, char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_attestor) *out_attestor = NULL;
    if (!safe_identity(name, 64u) || !safe_identity(key_id, 128u) ||
        max_signature_bytes == 0u ||
        max_signature_bytes > EGRESS_MAX_ATTESTATION_BYTES || !attest || !out_attestor) {
        egress_set_error(out_error,
            "attestor requires canonical identities, callback and signature bound <= %u",
            (unsigned int)EGRESS_MAX_ATTESTATION_BYTES);
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    maelys_egress_attestor_t *created = calloc(1, sizeof(*created));
    if (!created) return MAELYS_EGRESS_ERR_MEMORY;
    atomic_init(&created->references, 1u);
    (void)snprintf(created->name, sizeof(created->name), "%s", name);
    (void)snprintf(created->key_id, sizeof(created->key_id), "%s", key_id);
    created->max_signature_bytes = max_signature_bytes;
    created->attest = attest;
    created->context = context;
    created->release_context = release_context;
    *out_attestor = created;
    return MAELYS_EGRESS_OK;
}

void maelys_egress_attestor_retain(maelys_egress_attestor_t *attestor) {
    if (attestor) (void)atomic_fetch_add(&attestor->references, 1u);
}

void maelys_egress_attestor_release(maelys_egress_attestor_t *attestor) {
    if (!attestor || atomic_fetch_sub(&attestor->references, 1u) != 1u) return;
    if (attestor->release_context) attestor->release_context(attestor->context);
    free(attestor);
}
