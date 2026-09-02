#include "src/internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void exercise(const uint8_t *data, size_t size, int authenticated) {
    maelys_egress_config_t config;
    memset(&config, 0, sizeof(config));
    if (authenticated) {
        (void)snprintf(config.principals[0].username,
                       sizeof(config.principals[0].username), "maelys");
        (void)snprintf(config.principals[0].secret,
                       sizeof(config.principals[0].secret), "0123456789abcdef");
        config.principal_count = 1u;
        config.authentication_set = 1;
    } else {
        config.unauthenticated_loopback = 1;
    }
    int phase = 0;
    char invocation_id[EGRESS_MAX_INVOCATION_ID + 1u] = {0};
    size_t principal_index = SIZE_MAX;
    size_t offset = 0u;
    for (unsigned int step = 0u; step < 3u && offset <= size; ++step) {
        size_t consumed = 0u;
        unsigned char response[10];
        size_t response_length = 0u;
        egress_proxy_request_t request;
        int result = egress_parse_socks_frame(
            data + offset, size - offset, &config, &phase, &consumed,
            response, &response_length, &request, invocation_id, &principal_index);
        if (response_length > sizeof(response) || consumed > size - offset) abort();
        if (result != 1 || consumed == 0u) break;
        offset += consumed;
    }
    for (int direct_phase = 0; direct_phase <= 2; ++direct_phase) {
        int candidate = direct_phase;
        size_t consumed = 0u;
        unsigned char response[10];
        size_t response_length = 0u;
        egress_proxy_request_t request;
        (void)egress_parse_socks_frame(data, size, &config, &candidate, &consumed,
            response, &response_length, &request, invocation_id, &principal_index);
        if (response_length > sizeof(response) || consumed > size) abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    exercise(data, size, 0);
    exercise(data, size, 1);
    return 0;
}

#ifdef MAELYS_FUZZ_STANDALONE
int main(void) {
    static const unsigned char greeting[] = {5u, 2u, 0u, 2u};
    static const unsigned char request[] = {
        5u, 1u, 0u, 3u, 11u, 'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm',
        1u, 187u};
    (void)LLVMFuzzerTestOneInput(greeting, sizeof(greeting));
    (void)LLVMFuzzerTestOneInput(request, sizeof(request));
    unsigned char bytes[256];
    for (size_t i = 0u; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)i;
    for (size_t length = 0u; length <= sizeof(bytes); ++length) {
        (void)LLVMFuzzerTestOneInput(bytes, length);
    }
    return 0;
}
#endif
