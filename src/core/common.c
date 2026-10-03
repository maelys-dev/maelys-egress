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

/* A label a resolver reads as a number: decimal digits only, or 0x / 0X and
 * at least one hexadecimal digit. Judged on the bytes received, before any
 * change of case. */
static int numeric_label(const char *label, size_t length) {
    if (length == 0u) return 0;
    size_t first = 0u;
    int hexadecimal = length > 2u && label[0] == '0' && (label[1] == 'x' || label[1] == 'X');
    if (hexadecimal) first = 2u;
    for (size_t i = first; i < length; ++i) {
        unsigned char byte = (unsigned char)label[i];
        int digit = byte >= '0' && byte <= '9';
        int hex = (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
        if (!(digit || (hexadecimal && hex))) return 0;
    }
    return 1;
}

/* Exactly four decimal octets of 0 to 255, without a leading zero except
 * for 0 itself: the one numeric form every resolver reads the same way. */
static int strict_ipv4(const char *input, size_t length) {
    size_t octets = 0u, start = 0u;
    for (size_t i = 0u; i <= length; ++i) {
        if (i < length && input[i] != '.') continue;
        size_t size = i - start;
        if (size == 0u || size > 3u || (size > 1u && input[start] == '0')) return 0;
        unsigned int value = 0u;
        for (size_t j = start; j < i; ++j) {
            if (input[j] < '0' || input[j] > '9') return 0;
            value = value * 10u + (unsigned int)(input[j] - '0');
        }
        if (value > 255u || ++octets > 4u) return 0;
        start = i + 1u;
    }
    return octets == 4u;
}

/* A canonical host is either an IPv6 literal, kept as written, or a name:
 * 1 to 253 bytes of ASCII letters, digits and hyphens in labels of 1 to 63
 * bytes, no label beginning or ending with a hyphen, no trailing dot,
 * letters lowered. A name whose last label is numeric must be a strict IPv4
 * literal: 127.1, 2130706433, 0x7f.1 or 010.0.0.1 are refused, since the
 * system resolver reads them as addresses, and not the same address on
 * every host (010.0.0.1 is 10.0.0.1 on macOS and 8.0.0.1 with glibc and
 * musl). No inet_pton for IPv4: it accepts 010.0.0.1 on some systems. The
 * grammar is maelys-sandbox-policy's, so that both say the same. */
int egress_canonical_host(const char *input, char output[EGRESS_MAX_HOST + 1u]) {
    if (!input || !output) return 0;
    size_t length = strlen(input);
    if (length == 0u || length > EGRESS_MAX_HOST) return 0;
    struct in6_addr ipv6;
    if (memchr(input, ':', length) && inet_pton(AF_INET6, input, &ipv6) == 1) {
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
    if (numeric_label(input + label_start, final_length) && !strict_ipv4(input, length))
        return 0;
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
        static const unsigned char nat64[12] = {
            0x00, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0};
        if (memcmp(b, nat64, sizeof(nat64)) == 0) {
            uint32_t v4;
            memcpy(&v4, b + 12, sizeof(v4));
            return ipv4_private(v4);
        }
        /* Current global unicast space is 2000::/3. Unknown space fails closed. */
        if ((b[0] & 0xe0u) != 0x20u) return 1;
        if ((b[0] == 0x20u && b[1] == 0x01u && b[2] <= 0x01u) ||
            (b[0] == 0x20u && b[1] == 0x02u) ||
            (b[0] == 0x3fu && b[1] == 0xffu && (b[2] & 0xf0u) == 0u)) return 1;
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
unsigned int maelys_egress_abi_compatible_since(void) {
    return MAELYS_EGRESS_ABI_COMPATIBLE_SINCE;
}

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
