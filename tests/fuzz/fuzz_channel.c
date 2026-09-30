/*
 * Fuzz the channel codec: every input goes through both decoders, and a
 * datagram that decodes must re-encode to the same bytes. The target links
 * the codec object alone.
 */
#include "maelys/egress_channel.h"
#include "common/bootstrap.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (egress_bootstrap_decode_request(data, size) == EGRESS_BOOTSTRAP_OK) {
        unsigned char encoded[EGRESS_BOOTSTRAP_REQUEST_SIZE];
        egress_bootstrap_request(encoded);
        if (size != sizeof(encoded) || memcmp(data, encoded, size)) abort();
    }
    unsigned bootstrap_status; uint64_t timeout;
    if (egress_bootstrap_decode_response(data, size, &bootstrap_status, &timeout) &&
        data[5] <= EGRESS_BOOTSTRAP_INTERNAL && data[6] == 1u) {
        unsigned char encoded[EGRESS_BOOTSTRAP_RESPONSE_SIZE];
        egress_bootstrap_response(bootstrap_status, timeout, encoded);
        if (size != sizeof(encoded) || memcmp(data, encoded, size)) abort();
    }
    maelys_egress_channel_request_t request;
    const char *reason = NULL;
    if (maelys_egress_channel_decode_request(data, size, &request, &reason) ==
        MAELYS_EGRESS_CHANNEL_OK) {
        unsigned char again[MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE];
        size_t written = maelys_egress_channel_encode_request(
            request.host, request.port, again, sizeof(again));
        if (written != size || memcmp(again, data, size) != 0) abort();
    } else if (!reason) {
        abort();
    }
    maelys_egress_channel_response_t response;
    if (maelys_egress_channel_decode_response(data, size, &response, &reason)) {
        if (response.version == MAELYS_EGRESS_CHANNEL_PROTOCOL_VERSION &&
            response.reserved == 0u && response.status <= MAELYS_EGRESS_CHANNEL_INTERNAL) {
            unsigned char again[MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE];
            if (maelys_egress_channel_encode_response(
                    (maelys_egress_channel_status_t)response.status, again, sizeof(again)) !=
                    MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE ||
                memcmp(again, data, sizeof(again)) != 0) {
                abort();
            }
        }
    } else if (!reason) {
        abort();
    }
    return 0;
}

#ifdef MAELYS_FUZZ_STANDALONE
#include "tests/fuzz/standalone.h"

int main(int argc, char **argv) {
    if (fuzz_feed_arguments(argc, argv) != 0) return 1;
    static const unsigned char empty[] = "";
    static const unsigned char request[] = "MECQ\x01\x01\x01\xbb\x00\x0bexample.com";
    static const unsigned char response[] = "MECP\x01\x00\x00\x00";
    (void)LLVMFuzzerTestOneInput(empty, sizeof(empty) - 1u);
    (void)LLVMFuzzerTestOneInput(request, sizeof(request) - 1u);
    (void)LLVMFuzzerTestOneInput(response, sizeof(response) - 1u);
    unsigned char bootstrap_request[8], bootstrap_response[16];
    egress_bootstrap_request(bootstrap_request);
    for (size_t split = 0u; split <= sizeof(bootstrap_request); ++split)
        (void)LLVMFuzzerTestOneInput(bootstrap_request, split);
    for (unsigned status = 0u; status < 256u; ++status) {
        egress_bootstrap_response(status, 5000u, bootstrap_response);
        for (size_t split = 0u; split <= sizeof(bootstrap_response); ++split)
            (void)LLVMFuzzerTestOneInput(bootstrap_response, split);
    }
    unsigned char bytes[300];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)i;
    for (size_t length = 0; length <= sizeof(bytes); ++length) {
        (void)LLVMFuzzerTestOneInput(bytes, length);
    }
    return 0;
}
#endif
