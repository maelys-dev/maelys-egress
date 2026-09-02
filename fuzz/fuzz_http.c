#include "src/internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    maelys_egress_config_t config;
    memset(&config, 0, sizeof(config));
    (void)snprintf(config.principals[0].username,
                   sizeof(config.principals[0].username), "maelys");
    (void)snprintf(config.principals[0].secret,
                   sizeof(config.principals[0].secret), "0123456789abcdef");
    config.principal_count = 1u;
    config.authentication_set = 1;
    egress_proxy_request_t request;
    memset(&request, 0, sizeof(request));
    char *error = NULL;
    (void)egress_parse_http_request(data, size, &config, &request, &error);
    egress_proxy_request_clear(&request);
    free(error);
    return 0;
}

#ifdef MAELYS_FUZZ_STANDALONE
int main(void) {
    static const unsigned char empty[] = "";
    static const unsigned char connect[] = "CONNECT x:443 HTTP/1.1\r\n\r\n";
    static const unsigned char get[] = "GET http://x/ HTTP/1.1\r\nHost: x\r\n\r\n";
    static const unsigned char socks[] = "\x05\x01\x00";
    static const unsigned char bare_lf[] = "GET http://x/ HTTP/1.1\nHost: x\n\n";
    (void)LLVMFuzzerTestOneInput(empty, sizeof(empty) - 1u);
    (void)LLVMFuzzerTestOneInput(connect, sizeof(connect) - 1u);
    (void)LLVMFuzzerTestOneInput(get, sizeof(get) - 1u);
    (void)LLVMFuzzerTestOneInput(socks, sizeof(socks) - 1u);
    (void)LLVMFuzzerTestOneInput(bare_lf, sizeof(bare_lf) - 1u);
    unsigned char bytes[256];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)i;
    for (size_t length = 0; length <= sizeof(bytes); ++length) {
        (void)LLVMFuzzerTestOneInput(bytes, length);
    }
    return 0;
}
#endif
