#include "cli/cli.h"

/* Compiled once per binary. The plain maelys-egress build has no factory;
 * maelys-egress-mbedtls and maelys-egress-wolfssl define
 * MAELYS_EGRESS_TLS_FACTORY to their module constructor. Every other CLI
 * file asks this module at run time instead of carrying the preprocessor
 * condition itself. */

int egress_cli_tls_listener_supported(void) {
#ifdef MAELYS_EGRESS_TLS_FACTORY
    return 1;
#else
    return 0;
#endif
}

maelys_egress_result_t egress_cli_tls_listener_create(
    const maelys_egress_tls_files_t *files,
    maelys_egress_tls_provider_t **out_provider,
    char **out_error) {
#ifdef MAELYS_EGRESS_TLS_FACTORY
    return MAELYS_EGRESS_TLS_FACTORY(files, out_provider, out_error);
#else
    (void)files;
    if (out_provider) *out_provider = NULL;
    if (out_error) *out_error = NULL;
    return MAELYS_EGRESS_ERR_UNSUPPORTED;
#endif
}
