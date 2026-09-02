#include "src/internal.h"

#include <stdlib.h>
#include <string.h>

maelys_egress_result_t maelys_egress_tls_provider_create(
    const maelys_egress_tls_ops_t *ops,
    void *context,
    void (*release_context)(void *context),
    maelys_egress_tls_provider_t **out_provider,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_provider) *out_provider = NULL;
    if (!ops || !out_provider || !ops->name || !ops->name[0] ||
        strlen(ops->name) > 63u) {
        egress_set_error(out_error, "TLS provider metadata and output are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    for (const unsigned char *p = (const unsigned char *)ops->name; *p; ++p) {
        if (*p < 0x21u || *p > 0x7eu) {
            egress_set_error(out_error, "TLS provider name must be visible ASCII");
            return MAELYS_EGRESS_ERR_ARGUMENT;
        }
    }
    if (ops->abi_version != MAELYS_EGRESS_TLS_ABI_VERSION) {
        egress_set_error(out_error, "TLS provider %s uses ABI %u, Egress requires ABI %u",
                       ops->name, ops->abi_version,
                       (unsigned int)MAELYS_EGRESS_TLS_ABI_VERSION);
        return MAELYS_EGRESS_ERR_UNSUPPORTED;
    }
    if (!ops->session_create || !ops->handshake || !ops->read || !ops->write ||
        !ops->shutdown || !ops->last_error || !ops->session_release) {
        egress_set_error(out_error, "complete TLS provider operations are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    maelys_egress_tls_provider_t *provider = calloc(1, sizeof(*provider));
    if (!provider) return MAELYS_EGRESS_ERR_MEMORY;
    atomic_init(&provider->references, 1u);
    provider->ops = *ops;
    provider->name = egress_strdup(ops->name);
    if (!provider->name) {
        free(provider);
        return MAELYS_EGRESS_ERR_MEMORY;
    }
    provider->ops.name = provider->name;
    provider->context = context;
    provider->release_context = release_context;
    *out_provider = provider;
    return MAELYS_EGRESS_OK;
}

const char *maelys_egress_tls_provider_name(
    const maelys_egress_tls_provider_t *provider) {
    return provider ? provider->ops.name : NULL;
}

void maelys_egress_tls_provider_retain(maelys_egress_tls_provider_t *provider) {
    if (provider) (void)atomic_fetch_add(&provider->references, 1u);
}

void maelys_egress_tls_provider_release(maelys_egress_tls_provider_t *provider) {
    if (!provider) return;
    if (atomic_fetch_sub(&provider->references, 1u) != 1u) return;
    if (provider->release_context) provider->release_context(provider->context);
    free(provider->name);
    free(provider);
}
