#include "src/internal.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

int egress_parse_socks_frame(
    const unsigned char *bytes,
    size_t length,
    const maelys_egress_config_t *config,
    int *phase,
    size_t *out_consumed,
    unsigned char out_response[10],
    size_t *out_response_length,
    egress_proxy_request_t *out_request,
    char authenticated_invocation_id[EGRESS_MAX_INVOCATION_ID + 1u],
    size_t *authenticated_principal_index) {
    if (!bytes || !config || !phase || !out_consumed || !out_response ||
        !out_response_length || !out_request || *phase < 0 || *phase > 2) {
        return -1;
    }
    *out_consumed = 0u;
    *out_response_length = 0u;
    memset(out_request, 0, sizeof(*out_request));
    out_request->principal_index = SIZE_MAX;
    if (*phase == 0) {
        if (length < 2u) return 0;
        if (bytes[0] != 5u || bytes[1] == 0u) return -1;
        size_t needed = 2u + (size_t)bytes[1];
        if (length < needed) return 0;
        int endpoint_bound = egress_unix_principal_lookup(
            config, authenticated_invocation_id,
            authenticated_principal_index);
        unsigned char wanted = endpoint_bound ? 0u :
            config->authentication_set ? 2u : 0u;
        int found = 0;
        for (size_t i = 0; i < (size_t)bytes[1]; ++i) {
            if (bytes[2u + i] == wanted) found = 1;
        }
        out_response[0] = 5u;
        out_response[1] = found ? wanted : (unsigned char)0xffu;
        *out_response_length = 2u;
        *out_consumed = needed;
        if (!found) return -1;
        *phase = wanted == 2u ? 1 : 2;
        return 1;
    }
    if (*phase == 1) {
        if (length < 2u) return 0;
        if (bytes[0] != 1u) return -1;
        size_t user_length = bytes[1];
        if (user_length == 0u) return -1;
        if (length < 3u + user_length) return 0;
        size_t secret_length = bytes[2u + user_length];
        size_t needed = 3u + user_length + secret_length;
        if (secret_length == 0u) return -1;
        if (length < needed) return 0;
        char username[EGRESS_MAX_USERNAME + 1u];
        char secret[EGRESS_MAX_SECRET + 1u];
        int valid_sizes = user_length <= EGRESS_MAX_USERNAME &&
            secret_length <= EGRESS_MAX_SECRET;
        if (valid_sizes) {
            memcpy(username, bytes + 2u, user_length);
            username[user_length] = '\0';
            memcpy(secret, bytes + 3u + user_length, secret_length);
            secret[secret_length] = '\0';
        } else {
            username[0] = '\0';
            secret[0] = '\0';
        }
        int authenticated = valid_sizes &&
            egress_credentials_lookup(config, username, secret,
                                    authenticated_invocation_id,
                                    authenticated_principal_index);
        egress_secure_zero(secret, sizeof(secret));
        out_response[0] = 1u;
        out_response[1] = authenticated ? (unsigned char)0u : (unsigned char)1u;
        *out_response_length = 2u;
        *out_consumed = needed;
        if (!authenticated) return -2;
        *phase = 2;
        return 1;
    }
    if (length < 4u) return 0;
    if (bytes[0] != 5u || bytes[1] != 1u || bytes[2] != 0u) return -1;
    size_t host_offset = 4u;
    size_t host_length = 0u;
    char raw_host[EGRESS_MAX_HOST + 1u];
    if (bytes[3] == 1u) {
        if (length < 10u) return 0;
        if (!inet_ntop(AF_INET, bytes + 4u, raw_host, sizeof(raw_host))) return -1;
        host_length = 4u;
    } else if (bytes[3] == 4u) {
        if (length < 22u) return 0;
        if (!inet_ntop(AF_INET6, bytes + 4u, raw_host, sizeof(raw_host))) return -1;
        host_length = 16u;
    } else if (bytes[3] == 3u) {
        if (length < 5u) return 0;
        host_length = bytes[4];
        host_offset = 5u;
        if (host_length == 0u || host_length > EGRESS_MAX_HOST) return -1;
        if (length < host_offset + host_length + 2u) return 0;
        memcpy(raw_host, bytes + host_offset, host_length);
        raw_host[host_length] = '\0';
    } else {
        return -1;
    }
    size_t port_offset = host_offset + host_length;
    if (length < port_offset + 2u) return 0;
    out_request->protocol = MAELYS_EGRESS_PROTOCOL_SOCKS5;
    out_request->port = (uint16_t)(((uint16_t)bytes[port_offset] << 8u) |
                                  bytes[port_offset + 1u]);
    out_request->consumed = port_offset + 2u;
    (void)snprintf(out_request->invocation_id, sizeof(out_request->invocation_id),
                   "%s", authenticated_invocation_id);
    if (authenticated_principal_index) {
        out_request->principal_index = *authenticated_principal_index;
    }
    if (out_request->port == 0u ||
        !egress_canonical_host(raw_host, out_request->host)) return -1;
    *out_consumed = out_request->consumed;
    return 2;
}
