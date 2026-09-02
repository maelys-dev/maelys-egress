#include "src/internal.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void egress_set_error(char **out_error, const char *format, ...) {
    if (!out_error) return;
    free(*out_error);
    *out_error = NULL;
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed >= 0) {
        *out_error = malloc((size_t)needed + 1u);
        if (*out_error) {
            (void)vsnprintf(*out_error, (size_t)needed + 1u, format, arguments);
        }
    }
    va_end(arguments);
}

char *egress_strdup(const char *value) {
    if (!value) return NULL;
    size_t length = strlen(value);
    char *copy = malloc(length + 1u);
    if (copy) memcpy(copy, value, length + 1u);
    return copy;
}

void egress_secure_zero(void *data, size_t length) {
    volatile unsigned char *bytes = data;
    while (length) {
        *bytes++ = 0u;
        --length;
    }
}

int egress_canonical_host(const char *input, char output[EGRESS_MAX_HOST + 1u]) {
    if (!input || !output) return 0;
    size_t length = strlen(input);
    if (length == 0u || length > EGRESS_MAX_HOST) return 0;
    struct in_addr ipv4;
    struct in6_addr ipv6;
    if (inet_pton(AF_INET, input, &ipv4) == 1 ||
        inet_pton(AF_INET6, input, &ipv6) == 1) {
        memcpy(output, input, length + 1u);
        return 1;
    }
    size_t label_start = 0u;
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)input[i];
        if (byte == '.') {
            size_t label_length = i - label_start;
            if (label_length == 0u || label_length > 63u ||
                output[label_start] == '-' || output[i - 1u] == '-') return 0;
            output[i] = '.';
            label_start = i + 1u;
            continue;
        }
        int ascii_alnum = (byte >= 'A' && byte <= 'Z') ||
            (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
        if (!(ascii_alnum || byte == '-')) return 0;
        output[i] = byte >= 'A' && byte <= 'Z' ?
            (char)(byte + ('a' - 'A')) : (char)byte;
    }
    size_t final_length = length - label_start;
    if (final_length == 0u || final_length > 63u ||
        output[label_start] == '-' || output[length - 1u] == '-') return 0;
    output[length] = '\0';
    return 1;
}

int egress_constant_time_equal(const char *left, const char *right) {
    if (!left || !right) return 0;
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    size_t maximum = left_length > right_length ? left_length : right_length;
    unsigned int difference = (unsigned int)(left_length ^ right_length);
    for (size_t i = 0; i < maximum; ++i) {
        unsigned char a = i < left_length ? (unsigned char)left[i] : 0u;
        unsigned char b = i < right_length ? (unsigned char)right[i] : 0u;
        difference |= (unsigned int)(a ^ b);
    }
    return difference == 0u;
}

static int ipv4_private(uint32_t address) {
    uint32_t host = ntohl(address);
    unsigned int first = host >> 24u;
    unsigned int second = (host >> 16u) & 0xffu;
    if (first == 0u || first == 10u || first == 127u || first >= 224u) return 1;
    if (first == 100u && second >= 64u && second <= 127u) return 1;
    if (first == 169u && second == 254u) return 1;
    if (first == 172u && second >= 16u && second <= 31u) return 1;
    if (first == 192u && (second == 0u || second == 168u)) return 1;
    if (first == 192u && second == 88u) return 1;
    if (first == 198u && (second == 18u || second == 19u)) return 1;
    if ((first == 192u && second == 0u) ||
        (first == 198u && second == 51u) ||
        (first == 203u && second == 0u)) return 1;
    return 0;
}

int egress_address_is_private(const struct sockaddr *address) {
    if (!address) return 1;
    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)address;
        return ipv4_private(ipv4->sin_addr.s_addr);
    }
    if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)address;
        const unsigned char *b = ipv6->sin6_addr.s6_addr;
        static const unsigned char mapped[12] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, mapped, sizeof(mapped)) == 0) {
            uint32_t v4;
            memcpy(&v4, b + 12, sizeof(v4));
            return ipv4_private(v4);
        }
        if ((b[0] & 0xfeu) == 0xfcu ||
            (b[0] == 0xfeu && (b[1] & 0xc0u) == 0x80u) ||
            b[0] == 0xffu) return 1;
        if ((b[0] == 0x01u && b[1] == 0x00u && b[2] == 0u && b[3] == 0u &&
             b[4] == 0u && b[5] == 0u && b[6] == 0u && b[7] == 0u) ||
            (b[0] == 0x20u && b[1] == 0x01u && b[2] <= 0x01u) ||
            (b[0] == 0x20u && b[1] == 0x02u) ||
            (b[0] == 0x3fu && (b[1] & 0xf0u) == 0xf0u) ||
            (b[0] == 0x5fu && b[1] == 0x00u)) return 1;
        static const unsigned char loopback[16] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        static const unsigned char unspecified[16] = {0};
        if (memcmp(b, loopback, sizeof(loopback)) == 0 ||
            memcmp(b, unspecified, sizeof(unspecified)) == 0) return 1;
        if (b[0] == 0x20u && b[1] == 0x01u && b[2] == 0x0du && b[3] == 0xb8u) {
            return 1;
        }
        return 0;
    }
    return 1;
}

const char *maelys_egress_version_string(void) {
    return MAELYS_EGRESS_BUILD_VERSION;
}

unsigned int maelys_egress_abi_version(void) { return MAELYS_EGRESS_ABI_VERSION; }

const char *maelys_egress_result_string(maelys_egress_result_t result) {
    switch (result) {
        case MAELYS_EGRESS_OK: return "ok";
        case MAELYS_EGRESS_ERR_ARGUMENT: return "invalid argument";
        case MAELYS_EGRESS_ERR_MEMORY: return "out of memory";
        case MAELYS_EGRESS_ERR_STATE: return "invalid state";
        case MAELYS_EGRESS_ERR_IO: return "I/O error";
        case MAELYS_EGRESS_ERR_DENIED: return "denied";
        case MAELYS_EGRESS_ERR_PROTOCOL: return "protocol error";
        case MAELYS_EGRESS_ERR_TIMEOUT: return "timeout";
        case MAELYS_EGRESS_ERR_CANCELLED: return "cancelled";
        case MAELYS_EGRESS_ERR_UNSUPPORTED: return "unsupported";
        case MAELYS_EGRESS_ERR_CRYPTO: return "crypto";
    }
    return "unknown";
}

void maelys_egress_error_free(char *error) { free(error); }
