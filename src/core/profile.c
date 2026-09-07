#include "maelys/egress_profile.h"
#include "src/internal.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROFILE_ENV_COUNT 8u
#define PROFILE_URL_MAX 1024u

struct maelys_egress_profile {
    char username[EGRESS_MAX_USERNAME + 1u];
    char secret[EGRESS_MAX_SECRET + 1u];
    char invocation_id[EGRESS_MAX_INVOCATION_ID + 1u];
    char http_url[PROFILE_URL_MAX];
    char socks_url[PROFILE_URL_MAX];
    int endpoint_bound;
};

static int append_percent_encoded(
    char *output, size_t capacity, size_t *used, const char *input) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)input; *p; ++p) {
        int unreserved = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' ||
            *p == '_' || *p == '~';
        size_t needed = unreserved ? 1u : 3u;
        if (*used >= capacity || needed >= capacity - *used) return 0;
        if (unreserved) {
            output[(*used)++] = (char)*p;
        } else {
            output[(*used)++] = '%';
            output[(*used)++] = hex[*p >> 4u];
            output[(*used)++] = hex[*p & 0x0fu];
        }
    }
    output[*used] = '\0';
    return 1;
}

static int make_url(
    char output[PROFILE_URL_MAX], const char *scheme, const char *host,
    uint16_t port, const char *username, const char *secret) {
    struct in6_addr ipv6;
    int bracket = inet_pton(AF_INET6, host, &ipv6) == 1;
    int prefix = snprintf(output, PROFILE_URL_MAX, "%s://", scheme);
    if (prefix < 0 || (size_t)prefix >= PROFILE_URL_MAX) return 0;
    size_t used = (size_t)prefix;
    if (username || secret) {
        if (!username || !secret ||
            !append_percent_encoded(output, PROFILE_URL_MAX, &used, username) ||
            used + 1u >= PROFILE_URL_MAX) return 0;
        output[used++] = ':'; output[used] = '\0';
        if (!append_percent_encoded(output, PROFILE_URL_MAX, &used, secret) ||
            used + 2u >= PROFILE_URL_MAX) return 0;
        output[used++] = '@'; output[used] = '\0';
    }
    int suffix = snprintf(output + used, PROFILE_URL_MAX - used,
        bracket ? "[%s]:%u" : "%s:%u", host, (unsigned int)port);
    return suffix >= 0 && (size_t)suffix < PROFILE_URL_MAX - used;
}

maelys_egress_result_t maelys_egress_profile_create_endpoint_bound(
    const char *numeric_proxy_host,
    uint16_t proxy_port,
    const char *principal_name,
    const char *invocation_id,
    maelys_egress_profile_t **out_profile,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_profile) *out_profile = NULL;
    struct in_addr ipv4;
    struct in6_addr ipv6;
    if (!numeric_proxy_host || proxy_port == 0u || !principal_name ||
        !invocation_id || !out_profile ||
        (inet_pton(AF_INET, numeric_proxy_host, &ipv4) != 1 &&
         inet_pton(AF_INET6, numeric_proxy_host, &ipv6) != 1)) {
        egress_set_error(out_error,
            "numeric proxy endpoint, principal and invocation id are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    maelys_egress_config_t scratch;
    memset(&scratch, 0, sizeof(scratch));
    scratch.listen_unix = 1;
    scratch.unix_peer_policy = MAELYS_EGRESS_UNIX_PEER_SAME_EUID;
    maelys_egress_result_t result = maelys_egress_config_set_unix_principal(
        &scratch, principal_name, invocation_id, out_error);
    if (result != MAELYS_EGRESS_OK) return result;
    maelys_egress_profile_t *profile = calloc(1, sizeof(*profile));
    if (!profile) return MAELYS_EGRESS_ERR_MEMORY;
    (void)snprintf(profile->username, sizeof(profile->username), "%s", principal_name);
    (void)snprintf(profile->invocation_id, sizeof(profile->invocation_id), "%s",
                   invocation_id);
    profile->endpoint_bound = 1;
    if (!make_url(profile->http_url, "http", numeric_proxy_host, proxy_port,
                  NULL, NULL) ||
        !make_url(profile->socks_url, "socks5h", numeric_proxy_host, proxy_port,
                  NULL, NULL)) {
        maelys_egress_profile_destroy(profile);
        egress_set_error(out_error, "encoded proxy URL exceeds its bound");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    *out_profile = profile;
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_profile_create(
    const char *numeric_proxy_host,
    uint16_t proxy_port,
    const char *username,
    const char *secret,
    const char *invocation_id,
    maelys_egress_profile_t **out_profile,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_profile) *out_profile = NULL;
    struct in_addr ipv4;
    struct in6_addr ipv6;
    if (!numeric_proxy_host || proxy_port == 0u || !username || !secret ||
        !invocation_id || !out_profile ||
        (inet_pton(AF_INET, numeric_proxy_host, &ipv4) != 1 &&
         inet_pton(AF_INET6, numeric_proxy_host, &ipv6) != 1)) {
        egress_set_error(out_error, "numeric proxy endpoint, credentials and invocation id are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    maelys_egress_config_t scratch;
    memset(&scratch, 0, sizeof(scratch));
    maelys_egress_result_t result = maelys_egress_config_add_principal(
        &scratch, username, secret, invocation_id, out_error);
    if (result != MAELYS_EGRESS_OK) return result;
    maelys_egress_profile_t *profile = calloc(1, sizeof(*profile));
    if (!profile) {
        egress_secure_zero(&scratch, sizeof(scratch));
        return MAELYS_EGRESS_ERR_MEMORY;
    }
    (void)snprintf(profile->username, sizeof(profile->username), "%s", username);
    (void)snprintf(profile->secret, sizeof(profile->secret), "%s", secret);
    (void)snprintf(profile->invocation_id, sizeof(profile->invocation_id), "%s",
                   invocation_id);
    if (!make_url(profile->http_url, "http", numeric_proxy_host, proxy_port,
                  username, secret) ||
        !make_url(profile->socks_url, "socks5h", numeric_proxy_host, proxy_port,
                  username, secret)) {
        maelys_egress_profile_destroy(profile);
        egress_secure_zero(&scratch, sizeof(scratch));
        egress_set_error(out_error, "encoded proxy URL exceeds its bound");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    egress_secure_zero(&scratch, sizeof(scratch));
    *out_profile = profile;
    return MAELYS_EGRESS_OK;
}

void maelys_egress_profile_destroy(maelys_egress_profile_t *profile) {
    if (!profile) return;
    egress_secure_zero(profile, sizeof(*profile));
    free(profile);
}

size_t maelys_egress_profile_environment_count(const maelys_egress_profile_t *profile) {
    return profile ? PROFILE_ENV_COUNT : 0u;
}

maelys_egress_result_t maelys_egress_profile_environment_at(
    const maelys_egress_profile_t *profile,
    size_t index,
    const char **out_name,
    const char **out_value) {
    static const char *names[PROFILE_ENV_COUNT] = {
        "HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
        "ALL_PROXY", "all_proxy", "NO_PROXY", "no_proxy"
    };
    if (!profile || index >= PROFILE_ENV_COUNT || !out_name || !out_value) {
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    *out_name = names[index];
    *out_value = index < 4u ? profile->http_url : index < 6u ? profile->socks_url : "";
    return MAELYS_EGRESS_OK;
}

const char *maelys_egress_profile_invocation_id(
    const maelys_egress_profile_t *profile) {
    return profile ? profile->invocation_id : NULL;
}

maelys_egress_result_t maelys_egress_profile_apply(
    const maelys_egress_profile_t *profile,
    maelys_egress_config_t *config,
    char **out_error) {
    if (!profile || !config) return MAELYS_EGRESS_ERR_ARGUMENT;
    if (profile->endpoint_bound) {
        return maelys_egress_config_set_unix_principal(
            config, profile->username, profile->invocation_id, out_error);
    }
    return maelys_egress_config_add_principal(config, profile->username,
        profile->secret, profile->invocation_id, out_error);
}
