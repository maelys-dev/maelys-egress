#ifndef MAELYS_EGRESS_TLS_MODULES_H
#define MAELYS_EGRESS_TLS_MODULES_H

#include "maelys/egress_tls.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * File-backed reference-provider configuration. Paths are copied while the
 * provider is created. A server provider requires certificate_file and
 * private_key_file. A client provider requires ca_file. Supplying a client
 * certificate/key enables mutual TLS when the peer requests it.
 */
typedef struct maelys_egress_tls_files {
    const char *certificate_file;
    const char *private_key_file;
    const char *ca_file;
    int require_client_certificate;
} maelys_egress_tls_files_t;

maelys_egress_result_t maelys_egress_tls_mbedtls_create(
    const maelys_egress_tls_files_t *files,
    maelys_egress_tls_provider_t **out_provider,
    char **out_error);

maelys_egress_result_t maelys_egress_tls_wolfssl_create(
    const maelys_egress_tls_files_t *files,
    maelys_egress_tls_provider_t **out_provider,
    char **out_error);

#ifdef __cplusplus
}
#endif

#endif
