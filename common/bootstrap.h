/* SPDX-License-Identifier: MPL-2.0 */
/* Private, shared codec for protocol/egress-channel-bootstrap-v1.md. */
#ifndef EGRESS_BOOTSTRAP_H
#define EGRESS_BOOTSTRAP_H
#include <stddef.h>
#include <stdint.h>
#define EGRESS_BOOTSTRAP_REQUEST_SIZE 8u
#define EGRESS_BOOTSTRAP_RESPONSE_SIZE 16u
enum egress_bootstrap_status {
    EGRESS_BOOTSTRAP_OK, EGRESS_BOOTSTRAP_DENIED, EGRESS_BOOTSTRAP_BUSY,
    EGRESS_BOOTSTRAP_CANCELLED, EGRESS_BOOTSTRAP_MALFORMED,
    EGRESS_BOOTSTRAP_UNSUPPORTED, EGRESS_BOOTSTRAP_RESOURCE,
    EGRESS_BOOTSTRAP_INTERNAL
};
void egress_bootstrap_request(unsigned char out[EGRESS_BOOTSTRAP_REQUEST_SIZE]);
unsigned egress_bootstrap_decode_request(const unsigned char *bytes, size_t length);
void egress_bootstrap_response(unsigned status, uint64_t timeout,
    unsigned char out[EGRESS_BOOTSTRAP_RESPONSE_SIZE]);
int egress_bootstrap_decode_response(const unsigned char *bytes, size_t length,
    unsigned *status, uint64_t *timeout);
int egress_bootstrap_path_valid(const char *path);
#endif
