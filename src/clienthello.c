#include "src/internal.h"

#include <stdio.h>
#include <string.h>

static uint16_t read_u16(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0] << 8u) | (uint16_t)p[1]);
}

static size_t read_u24(const unsigned char *p) {
    return ((size_t)p[0] << 16u) | ((size_t)p[1] << 8u) | (size_t)p[2];
}

static int fail(char **out_error, const char *message) {
    egress_set_error(out_error, "TLS ClientHello rejected: %s", message);
    return -1;
}

int egress_tls_client_hello_matches(
    const unsigned char *bytes,
    size_t length,
    const char *expected_host,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!bytes || !expected_host) return fail(out_error, "missing input or expected host");
    unsigned char handshake[EGRESS_HANDSHAKE_MAX];
    size_t handshake_length = 0u;
    size_t offset = 0u;
    size_t needed = 4u;
    while (handshake_length < needed) {
        if (length - offset < 5u) return 0;
        if (bytes[offset] != 22u || bytes[offset + 1u] != 3u) {
            return fail(out_error, "first TLS flight is not a handshake record");
        }
        size_t record_length = read_u16(bytes + offset + 3u);
        if (record_length == 0u || record_length > EGRESS_HANDSHAKE_MAX) {
            return fail(out_error, "invalid record length");
        }
        if (length - offset - 5u < record_length) return 0;
        if (record_length > sizeof(handshake) - handshake_length) {
            return fail(out_error, "handshake exceeds the configured bound");
        }
        memcpy(handshake + handshake_length, bytes + offset + 5u, record_length);
        handshake_length += record_length;
        offset += 5u + record_length;
        if (handshake_length >= 4u) {
            if (handshake[0] != 1u) return fail(out_error, "first handshake is not ClientHello");
            size_t body_length = read_u24(handshake + 1u);
            if (body_length > EGRESS_HANDSHAKE_MAX - 4u) {
                return fail(out_error, "ClientHello exceeds the configured bound");
            }
            needed = body_length + 4u;
        }
    }

    const unsigned char *p = handshake + 4u;
    size_t remaining = needed - 4u;
    if (remaining < 2u + 32u + 1u) return fail(out_error, "truncated fixed fields");
    p += 34u; remaining -= 34u;
    size_t session_id_length = p[0]; p++; remaining--;
    if (session_id_length > remaining) return fail(out_error, "truncated session id");
    p += session_id_length; remaining -= session_id_length;
    if (remaining < 2u) return fail(out_error, "missing cipher suites");
    size_t cipher_length = read_u16(p); p += 2u; remaining -= 2u;
    if (cipher_length < 2u || (cipher_length & 1u) || cipher_length > remaining) {
        return fail(out_error, "invalid cipher suites");
    }
    p += cipher_length; remaining -= cipher_length;
    if (remaining < 1u) return fail(out_error, "missing compression methods");
    size_t compression_length = p[0]; p++; remaining--;
    if (compression_length == 0u || compression_length > remaining) {
        return fail(out_error, "invalid compression methods");
    }
    p += compression_length; remaining -= compression_length;
    if (remaining < 2u) return fail(out_error, "missing extensions");
    size_t extensions_length = read_u16(p); p += 2u; remaining -= 2u;
    if (extensions_length != remaining) return fail(out_error, "invalid extensions length");

    int seen_sni = 0;
    while (remaining) {
        if (remaining < 4u) return fail(out_error, "truncated extension");
        uint16_t type = read_u16(p);
        size_t extension_length = read_u16(p + 2u);
        p += 4u; remaining -= 4u;
        if (extension_length > remaining) return fail(out_error, "truncated extension body");
        if (type == 0xfe0du || type == 0xffceu) {
            return fail(out_error, "ECH is incompatible with cleartext SNI enforcement");
        }
        if (type == 0u) {
            if (seen_sni) return fail(out_error, "duplicate server_name extension");
            seen_sni = 1;
            if (extension_length < 2u || read_u16(p) != extension_length - 2u) {
                return fail(out_error, "invalid server_name list");
            }
            const unsigned char *name = p + 2u;
            size_t names_remaining = extension_length - 2u;
            int dns_names = 0;
            int total_names = 0;
            while (names_remaining) {
                if (names_remaining < 3u) return fail(out_error, "truncated server_name");
                unsigned int name_type = name[0];
                size_t name_length = read_u16(name + 1u);
                name += 3u; names_remaining -= 3u;
                if (name_length == 0u || name_length > names_remaining) {
                    return fail(out_error, "invalid server_name length");
                }
                ++total_names;
                if (name_type == 0u) {
                    char canonical[EGRESS_MAX_HOST + 1u];
                    char candidate[EGRESS_MAX_HOST + 1u];
                    if (++dns_names != 1 || name_length > EGRESS_MAX_HOST) {
                        return fail(out_error, "multiple or oversized DNS server names");
                    }
                    memcpy(candidate, name, name_length);
                    candidate[name_length] = '\0';
                    if (!egress_canonical_host(candidate, canonical) ||
                        strcmp(canonical, expected_host) != 0) {
                        return fail(out_error, "server_name does not match the sealed destination");
                    }
                }
                name += name_length; names_remaining -= name_length;
            }
            if (dns_names != 1 || total_names != 1) {
                return fail(out_error, "exactly one DNS server_name is required");
            }
        }
        p += extension_length; remaining -= extension_length;
    }
    if (!seen_sni) return fail(out_error, "server_name extension is required");
    return 1;
}
