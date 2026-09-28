/*
 * Fuzz the channel codec: every input goes through both decoders, and a
 * datagram that decodes must re-encode to the same bytes. The target links
 * the codec object alone.
 */
#include "maelys/egress_channel.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
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
    unsigned char bytes[300];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)i;
    for (size_t length = 0; length <= sizeof(bytes); ++length) {
        (void)LLVMFuzzerTestOneInput(bytes, length);
    }
    return 0;
}
#endif
