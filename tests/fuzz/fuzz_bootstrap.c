/* SPDX-License-Identifier: MPL-2.0 */
/* Only the two bootstrap codecs: no proxy, TLS, channel-v1 or OS transport. */
#include "common/bootstrap.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
static void decode(const uint8_t *data, size_t size) {
    if (egress_bootstrap_decode_request(data, size) == EGRESS_BOOTSTRAP_OK) {
        unsigned char encoded[EGRESS_BOOTSTRAP_REQUEST_SIZE];
        egress_bootstrap_request(encoded);
        if (size != sizeof(encoded) || memcmp(data, encoded, size)) abort();
    }
    unsigned status; uint64_t timeout;
    if (egress_bootstrap_decode_response(data, size, &status, &timeout) &&
        data[5] <= EGRESS_BOOTSTRAP_INTERNAL && data[6] == 1u) {
        unsigned char encoded[EGRESS_BOOTSTRAP_RESPONSE_SIZE];
        egress_bootstrap_response(status, timeout, encoded);
        if (size != sizeof(encoded) || memcmp(data, encoded, size)) abort();
    }
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    decode(data, size);
    /* Every fixed-frame prefix, including every possible stream split.
     * Actual stream reassembly is exercised in test_bootstrap_gates.c. */
    for (size_t split = 0u; split < size && split <= EGRESS_BOOTSTRAP_RESPONSE_SIZE; ++split)
        decode(data, split);
    return 0;
}
#ifdef MAELYS_FUZZ_STANDALONE
#include "tests/fuzz/standalone.h"
int main(int argc, char **argv) { return fuzz_feed_arguments(argc, argv); }
#endif
