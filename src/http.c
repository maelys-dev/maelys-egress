#include "src/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int token_char(unsigned char byte) {
    int ascii_alnum = (byte >= 'A' && byte <= 'Z') ||
        (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
    return ascii_alnum || strchr("!#$%&'*+-.^_`|~", byte) != NULL;
}

static int base64_char(unsigned char byte) {
    return (byte >= 'A' && byte <= 'Z') ||
        (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '+' || byte == '/';
}

static const unsigned char *find_bytes(
    const unsigned char *bytes,
    size_t length,
    const char *needle,
    size_t needle_length) {
    if (needle_length > length) return NULL;
    for (size_t i = 0; i + needle_length <= length; ++i) {
        if (memcmp(bytes + i, needle, needle_length) == 0) return bytes + i;
    }
    return NULL;
}

static int parse_port(const char *text, uint16_t *out_port) {
    if (!text || !*text) return 0;
    unsigned long value = 0u;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        value = value * 10u + (unsigned long)(*p - '0');
        if (value > 65535u) return 0;
    }
    if (value == 0u) return 0;
    *out_port = (uint16_t)value;
    return 1;
}

static int parse_authority(
    const char *authority,
    uint16_t default_port,
    char host[EGRESS_MAX_HOST + 1u],
    uint16_t *out_port) {
    if (!authority || !*authority || strchr(authority, '@')) return 0;
    char raw_host[EGRESS_MAX_HOST + 1u];
    const char *port_text = NULL;
    size_t host_length;
    if (authority[0] == '[') {
        const char *end = strchr(authority, ']');
        if (!end || (end[1] && end[1] != ':')) return 0;
        host_length = (size_t)(end - authority - 1);
        port_text = end[1] == ':' ? end + 2 : NULL;
        if (host_length == 0u || host_length > EGRESS_MAX_HOST) return 0;
        memcpy(raw_host, authority + 1, host_length);
    } else {
        const char *colon = strrchr(authority, ':');
        if (colon && strchr(authority, ':') == colon) {
            host_length = (size_t)(colon - authority);
            port_text = colon + 1;
        } else if (colon) {
            /* HTTP authorities require brackets around IPv6 literals. */
            return 0;
        } else {
            host_length = strlen(authority);
        }
        if (host_length == 0u || host_length > EGRESS_MAX_HOST) return 0;
        memcpy(raw_host, authority, host_length);
    }
    raw_host[host_length] = '\0';
    if (!egress_canonical_host(raw_host, host)) return 0;
    *out_port = default_port;
    return !port_text || parse_port(port_text, out_port);
}

static int decode_base64(const char *input, unsigned char *output, size_t capacity,
                         size_t *out_length) {
    static const signed char table[256] = {
        ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
        ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
        ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
        ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
        ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
        ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63
    };
    size_t length = strlen(input);
    if (length == 0u || length % 4u != 0u) return 0;
    size_t used = 0u;
    for (size_t i = 0; i < length; i += 4u) {
        unsigned char c0 = (unsigned char)input[i];
        unsigned char c1 = (unsigned char)input[i + 1u];
        unsigned char c2 = (unsigned char)input[i + 2u];
        unsigned char c3 = (unsigned char)input[i + 3u];
        if (!base64_char(c0) || !base64_char(c1)) return 0;
        if (c2 != '=' && !base64_char(c2)) return 0;
        if (c3 != '=' && !base64_char(c3)) return 0;
        if ((c2 == '=' && c3 != '=') || (c2 == '=' && i + 4u != length) ||
            (c3 == '=' && i + 4u != length)) return 0;
        if ((c2 == '=' && (((unsigned int)table[c1]) & 0x0fu) != 0u) ||
            (c3 == '=' && c2 != '=' &&
             (((unsigned int)table[c2]) & 0x03u) != 0u)) return 0;
        unsigned int value = ((unsigned int)table[c0] << 18u) |
            ((unsigned int)table[c1] << 12u) |
            ((unsigned int)(c2 == '=' ? 0 : table[c2]) << 6u) |
            (unsigned int)(c3 == '=' ? 0 : table[c3]);
        size_t count = c2 == '=' ? 1u : c3 == '=' ? 2u : 3u;
        if (used + count > capacity) return 0;
        output[used++] = (unsigned char)(value >> 16u);
        if (count > 1u) output[used++] = (unsigned char)(value >> 8u);
        if (count > 2u) output[used++] = (unsigned char)value;
    }
    *out_length = used;
    return 1;
}

int egress_credentials_match(
    const maelys_egress_config_t *config,
    const char *username,
    const char *secret) {
    char ignored[EGRESS_MAX_INVOCATION_ID + 1u];
    return egress_credentials_lookup(config, username, secret, ignored, NULL);
}

int egress_credentials_lookup(
    const maelys_egress_config_t *config,
    const char *username,
    const char *secret,
    char out_invocation_id[EGRESS_MAX_INVOCATION_ID + 1u],
    size_t *out_principal_index) {
    if (out_invocation_id) out_invocation_id[0] = '\0';
    if (out_principal_index) *out_principal_index = SIZE_MAX;
    if (!config || !config->authentication_set || !secret) return 0;
    size_t matched = SIZE_MAX;
    for (size_t i = 0; i < config->principal_count; ++i) {
        int username_matches = username ?
            egress_constant_time_equal(username, config->principals[i].username) : 1;
        int secret_matches = egress_constant_time_equal(
            secret, config->principals[i].secret);
        if (username_matches & secret_matches) matched = i;
    }
    if (matched == SIZE_MAX) return 0;
    if (out_invocation_id) {
        (void)snprintf(out_invocation_id, EGRESS_MAX_INVOCATION_ID + 1u, "%s",
            config->principals[matched].invocation_id);
    }
    if (out_principal_index) *out_principal_index = matched;
    return 1;
}

int egress_unix_principal_lookup(
    const maelys_egress_config_t *config,
    char out_invocation_id[EGRESS_MAX_INVOCATION_ID + 1u],
    size_t *out_principal_index) {
    if (out_invocation_id) out_invocation_id[0] = '\0';
    if (out_principal_index) *out_principal_index = SIZE_MAX;
    if (!config || !config->listen_unix || !config->unix_principal_bound ||
        config->unix_principal_index >= config->principal_count) return 0;
    const egress_principal_t *principal =
        &config->principals[config->unix_principal_index];
    if (out_invocation_id) {
        (void)snprintf(out_invocation_id, EGRESS_MAX_INVOCATION_ID + 1u, "%s",
                       principal->invocation_id);
    }
    if (out_principal_index) *out_principal_index = config->unix_principal_index;
    return 1;
}

static int auth_header_matches(
    const maelys_egress_config_t *config,
    const char *value,
    char out_invocation_id[EGRESS_MAX_INVOCATION_ID + 1u],
    size_t *out_principal_index) {
    if (!config->authentication_set) return config->unauthenticated_loopback;
    if (strncasecmp(value, "Bearer ", 7u) == 0) {
        return egress_credentials_lookup(config, NULL, value + 7u,
                                       out_invocation_id, out_principal_index);
    }
    if (strncasecmp(value, "Basic ", 6u) != 0) return 0;
    unsigned char decoded[EGRESS_MAX_USERNAME + EGRESS_MAX_SECRET + 2u];
    size_t decoded_length = 0u;
    if (!decode_base64(value + 6u, decoded, sizeof(decoded) - 1u,
                       &decoded_length)) return 0;
    decoded[decoded_length] = '\0';
    char *separator = strchr((char *)decoded, ':');
    if (!separator) {
        egress_secure_zero(decoded, sizeof(decoded));
        return 0;
    }
    *separator = '\0';
    int matches = egress_credentials_lookup(config, (char *)decoded, separator + 1u,
                                           out_invocation_id, out_principal_index);
    egress_secure_zero(decoded, sizeof(decoded));
    return matches;
}

static int append_bytes(
    unsigned char *output,
    size_t capacity,
    size_t *used,
    const void *bytes,
    size_t length) {
    if (*used + length > capacity) return 0;
    memcpy(output + *used, bytes, length);
    *used += length;
    return 1;
}

int egress_parse_http_request(
    const unsigned char *bytes,
    size_t length,
    const maelys_egress_config_t *config,
    egress_proxy_request_t *out_request,
    char **out_error) {
    if (!bytes || !config || !out_request) return -1;
    out_request->principal_index = SIZE_MAX;
    int endpoint_bound = egress_unix_principal_lookup(
        config, out_request->invocation_id, &out_request->principal_index);
    const unsigned char *header_end = find_bytes(bytes, length, "\r\n\r\n", 4u);
    if (!header_end) {
        if (length >= EGRESS_HANDSHAKE_MAX) {
            egress_set_error(out_error, "HTTP proxy header exceeds %u bytes",
                           (unsigned int)EGRESS_HANDSHAKE_MAX);
            return -1;
        }
        return 0;
    }
    size_t header_length = (size_t)(header_end - bytes) + 4u;
    for (size_t i = 0; i < header_length; ++i) {
        if (bytes[i] == 0u || (bytes[i] < 0x20u && bytes[i] != '\r' && bytes[i] != '\n' && bytes[i] != '\t')) {
            egress_set_error(out_error, "HTTP proxy header contains a control byte");
            return -1;
        }
        if (bytes[i] == '\n' && (i == 0u || bytes[i - 1u] != '\r')) {
            egress_set_error(out_error, "HTTP proxy requires CRLF framing");
            return -1;
        }
    }
    char *header = calloc(header_length + 1u, 1u);
    if (!header) return -1;
    memcpy(header, bytes, header_length);
    char *first_end = strstr(header, "\r\n");
    if (!first_end) { free(header); return -1; }
    *first_end = '\0';
    char *method = header;
    char *target = memchr(method, ' ', header_length);
    if (!target) { free(header); return -1; }
    *target++ = '\0';
    size_t target_capacity = header_length - (size_t)(target - header);
    char *version = memchr(target, ' ', target_capacity);
    if (!version) { free(header); return -1; }
    *version++ = '\0';
    if (strchr(version, ' ') || strcmp(version, "HTTP/1.1") != 0) {
        egress_set_error(out_error, "only HTTP/1.1 proxy requests are supported");
        free(header);
        return -1;
    }
    size_t method_length = strlen(method);
    if (method_length == 0u || method_length > 16u) { free(header); return -1; }
    for (size_t i = 0; i < method_length; ++i) {
        if (!token_char((unsigned char)method[i])) { free(header); return -1; }
    }
    int connect_method = strcmp(method, "CONNECT") == 0;
    char authority[EGRESS_MAX_HOST + 16u];
    const char *origin_target = NULL;
    if (connect_method) {
        if (strlen(target) >= sizeof(authority)) { free(header); return -1; }
        (void)snprintf(authority, sizeof(authority), "%s", target);
        if (!parse_authority(authority, 443u, out_request->host, &out_request->port)) {
            egress_set_error(out_error, "invalid CONNECT authority");
            free(header);
            return -1;
        }
        out_request->protocol = MAELYS_EGRESS_PROTOCOL_HTTP_CONNECT;
    } else {
        if (strncmp(target, "http://", 7u) != 0) {
            egress_set_error(out_error, "plain proxy requests require an absolute http:// URI");
            free(header);
            return -1;
        }
        const char *begin = target + 7u;
        const char *slash = strchr(begin, '/');
        const char *question = strchr(begin, '?');
        const char *path = slash;
        if (!path || (question && question < path)) path = question;
        size_t authority_length = path ? (size_t)(path - begin) : strlen(begin);
        if (authority_length == 0u || authority_length >= sizeof(authority)) {
            free(header); return -1;
        }
        memcpy(authority, begin, authority_length);
        authority[authority_length] = '\0';
        if (!parse_authority(authority, 80u, out_request->host, &out_request->port)) {
            egress_set_error(out_error, "invalid absolute HTTP authority");
            free(header);
            return -1;
        }
        origin_target = path ? path : "/";
        if (strchr(origin_target, '#')) {
            egress_set_error(out_error, "HTTP proxy URI fragments are forbidden");
            free(header); return -1;
        }
        out_request->protocol = MAELYS_EGRESS_PROTOCOL_HTTP_FORWARD;
    }

    int authenticated = 0;
    int auth_headers = 0;
    int host_headers = 0;
    int content_length_headers = 0;
    char header_host[EGRESS_MAX_HOST + 1u] = {0};
    uint16_t header_port = 0u;
    unsigned char *rewritten = NULL;
    size_t rewritten_used = 0u;
    size_t rewritten_capacity = header_length + 64u;
    if (!connect_method) {
        /* One extra byte is reserved for diagnostics/tests; wire length stays explicit. */
        rewritten = malloc(rewritten_capacity + 1u);
        if (!rewritten) { free(header); return -1; }
        int prefix = snprintf((char *)rewritten, rewritten_capacity, "%s %s HTTP/1.1\r\n",
                              method, origin_target);
        if (prefix < 0 || (size_t)prefix >= rewritten_capacity) {
            free(rewritten); free(header); return -1;
        }
        rewritten_used = (size_t)prefix;
    }
    char *line = first_end + 2u;
    while (*line) {
        char *end = strstr(line, "\r\n");
        if (!end) { free(rewritten); free(header); return -1; }
        if (end == line) break;
        *end = '\0';
        char *colon = strchr(line, ':');
        if (!colon || colon == line) { free(rewritten); free(header); return -1; }
        for (char *p = line; p < colon; ++p) {
            if (!token_char((unsigned char)*p)) { free(rewritten); free(header); return -1; }
        }
        *colon++ = '\0';
        while (*colon == ' ' || *colon == '\t') ++colon;
        size_t value_length = strlen(colon);
        while (value_length && (colon[value_length - 1u] == ' ' || colon[value_length - 1u] == '\t')) {
            colon[--value_length] = '\0';
        }
        if (strcasecmp(line, "Proxy-Authorization") == 0) {
            ++auth_headers;
            if (!endpoint_bound) {
                authenticated = auth_header_matches(config, colon,
                    out_request->invocation_id, &out_request->principal_index);
            }
        } else if (strcasecmp(line, "Host") == 0) {
            ++host_headers;
            if (!parse_authority(colon, connect_method ? 443u : 80u,
                                 header_host, &header_port)) {
                free(rewritten); free(header); return -1;
            }
            if (!connect_method && !append_bytes(rewritten, rewritten_capacity,
                &rewritten_used, line, strlen(line))) {
                free(rewritten); free(header); return -1;
            } else if (!connect_method &&
                (!append_bytes(rewritten, rewritten_capacity, &rewritten_used, ": ", 2u) ||
                 !append_bytes(rewritten, rewritten_capacity, &rewritten_used, colon, strlen(colon)) ||
                 !append_bytes(rewritten, rewritten_capacity, &rewritten_used, "\r\n", 2u))) {
                free(rewritten); free(header); return -1;
            }
        } else if (strcasecmp(line, "Proxy-Connection") == 0 ||
                   strcasecmp(line, "Connection") == 0 ||
                   strcasecmp(line, "Keep-Alive") == 0) {
            /* Replaced with a single close directive below. */
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            egress_set_error(out_error, "transfer-encoded forward proxy requests are unsupported");
            free(rewritten); free(header); return -1;
        } else if (strcasecmp(line, "Content-Length") == 0) {
            ++content_length_headers;
            if (content_length_headers > 1 || !*colon) {
                egress_set_error(out_error, "exactly one canonical Content-Length is permitted");
                free(rewritten); free(header); return -1;
            }
            for (const unsigned char *p = (const unsigned char *)colon; *p; ++p) {
                if (*p < '0' || *p > '9') {
                    egress_set_error(out_error, "Content-Length must be decimal");
                    free(rewritten); free(header); return -1;
                }
            }
            if (!connect_method &&
                (!append_bytes(rewritten, rewritten_capacity, &rewritten_used, line, strlen(line)) ||
                 !append_bytes(rewritten, rewritten_capacity, &rewritten_used, ": ", 2u) ||
                 !append_bytes(rewritten, rewritten_capacity, &rewritten_used, colon, strlen(colon)) ||
                 !append_bytes(rewritten, rewritten_capacity, &rewritten_used, "\r\n", 2u))) {
                free(rewritten); free(header); return -1;
            }
        } else if (!connect_method &&
            (!append_bytes(rewritten, rewritten_capacity, &rewritten_used, line, strlen(line)) ||
             !append_bytes(rewritten, rewritten_capacity, &rewritten_used, ": ", 2u) ||
             !append_bytes(rewritten, rewritten_capacity, &rewritten_used, colon, strlen(colon)) ||
             !append_bytes(rewritten, rewritten_capacity, &rewritten_used, "\r\n", 2u))) {
            free(rewritten); free(header); return -1;
        }
        line = end + 2u;
    }
    if ((endpoint_bound && auth_headers != 0) || auth_headers > 1 ||
        (!endpoint_bound && config->authentication_set &&
        (auth_headers != 1 || !authenticated))) {
        egress_set_error(out_error, "proxy authentication failed");
        free(rewritten); free(header); return -2;
    }
    if (!endpoint_bound && !config->authentication_set &&
        !config->unauthenticated_loopback) {
        egress_set_error(out_error, "proxy authentication is required");
        free(rewritten); free(header); return -2;
    }
    if (host_headers != 1 || strcmp(header_host, out_request->host) != 0 ||
        header_port != out_request->port) {
        egress_set_error(out_error, "Host header and request authority must match exactly");
        free(rewritten); free(header); return -1;
    }
    if (!connect_method) {
        if (!append_bytes(rewritten, rewritten_capacity, &rewritten_used,
                          "Connection: close\r\n\r\n", 21u)) {
            free(rewritten); free(header); return -1;
        }
        rewritten[rewritten_used] = '\0';
        out_request->forward_bytes = rewritten;
        out_request->forward_length = rewritten_used;
    }
    out_request->consumed = header_length;
    free(header);
    return 1;
}

void egress_proxy_request_clear(egress_proxy_request_t *request) {
    if (!request) return;
    free(request->forward_bytes);
    memset(request, 0, sizeof(*request));
}
