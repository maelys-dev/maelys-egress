/* SPDX-License-Identifier: MPL-2.0 */
#include "common/bootstrap.h"
#include <string.h>

void egress_bootstrap_request(unsigned char out[EGRESS_BOOTSTRAP_REQUEST_SIZE]) {
    memcpy(out, "MEBQ\1\1\0\0", EGRESS_BOOTSTRAP_REQUEST_SIZE);
}

unsigned egress_bootstrap_decode_request(const unsigned char *bytes, size_t length) {
    if (!bytes || length != EGRESS_BOOTSTRAP_REQUEST_SIZE ||
        memcmp(bytes, "MEBQ", 4u) || bytes[6] || bytes[7])
        return EGRESS_BOOTSTRAP_MALFORMED;
    return bytes[4] == 1u && bytes[5] == 1u ?
        EGRESS_BOOTSTRAP_OK : EGRESS_BOOTSTRAP_UNSUPPORTED;
}

void egress_bootstrap_response(unsigned status, uint64_t timeout,
    unsigned char out[EGRESS_BOOTSTRAP_RESPONSE_SIZE]) {
    memcpy(out, "MEBP\1\0\1\0", 8u);
    out[5] = (unsigned char)status;
    if (status != EGRESS_BOOTSTRAP_OK) timeout = 0u;
    for (size_t i = 0u; i < 8u; ++i)
        out[8u + i] = (unsigned char)(timeout >> (56u - 8u * i));
}

int egress_bootstrap_decode_response(const unsigned char *bytes, size_t length,
    unsigned *status, uint64_t *timeout) {
    if (!bytes || !status || !timeout) return 0;
    *status = EGRESS_BOOTSTRAP_INTERNAL;
    *timeout = 0u;
    if (length != EGRESS_BOOTSTRAP_RESPONSE_SIZE || memcmp(bytes, "MEBP", 4u) ||
        bytes[4] != 1u || bytes[7] ||
        (bytes[5] != EGRESS_BOOTSTRAP_UNSUPPORTED && bytes[6] != 1u)) return 0;
    uint64_t value = 0u;
    for (size_t i = 8u; i < 16u; ++i) value = (value << 8u) | bytes[i];
    if ((bytes[5] == EGRESS_BOOTSTRAP_OK) != (value != 0u)) return 0;
    *status = bytes[5] <= EGRESS_BOOTSTRAP_INTERNAL ? bytes[5] : EGRESS_BOOTSTRAP_INTERNAL;
    *timeout = value;
    return 1;
}

int egress_bootstrap_path_valid(const char *path) {
    if (!path || path[0] != '/' || !path[1]) return 0;
    for (const char *part = path + 1; ; ) {
        const char *end = strchr(part, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        if (!length || (length == 1u && part[0] == '.') ||
            (length == 2u && part[0] == '.' && part[1] == '.')) return 0;
        if (!end) return 1;
        part = end + 1;
    }
}
