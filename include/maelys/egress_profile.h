#ifndef MAELYS_EGRESS_PROFILE_H
#define MAELYS_EGRESS_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include "maelys/egress.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_egress_profile maelys_egress_profile_t;

maelys_egress_result_t maelys_egress_profile_create(
    const char *numeric_proxy_host,
    uint16_t proxy_port,
    const char *username,
    const char *secret,
    const char *invocation_id,
    maelys_egress_profile_t **out_profile,
    char **out_error);
/*
 * Build credential-free proxy environment for a configuration whose private
 * AF_UNIX listener is bound to principal_name. HTTP/SOCKS clients still use
 * their normal proxy environment, but the URLs contain no userinfo.
 */
maelys_egress_result_t maelys_egress_profile_create_endpoint_bound(
    const char *numeric_proxy_host,
    uint16_t proxy_port,
    const char *principal_name,
    const char *invocation_id,
    maelys_egress_profile_t **out_profile,
    char **out_error);
void maelys_egress_profile_destroy(maelys_egress_profile_t *profile);
size_t maelys_egress_profile_environment_count(
    const maelys_egress_profile_t *profile);
maelys_egress_result_t maelys_egress_profile_environment_at(
    const maelys_egress_profile_t *profile,
    size_t index,
    const char **out_name,
    const char **out_value);
const char *maelys_egress_profile_invocation_id(
    const maelys_egress_profile_t *profile);
maelys_egress_result_t maelys_egress_profile_apply(
    const maelys_egress_profile_t *profile,
    maelys_egress_config_t *config,
    char **out_error);

#ifdef __cplusplus
}
#endif

#endif
