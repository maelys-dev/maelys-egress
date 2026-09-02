#include "src/internal.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *error = NULL;
    (void)egress_tls_client_hello_matches(data, size, "example.com", &error);
    maelys_egress_error_free(error);
    return 0;
}

#ifdef MAELYS_FUZZ_STANDALONE
int main(void) {
    static const uint8_t corpus[][12] = {
        {0},
        {22u, 3u, 3u, 0u, 4u, 1u, 0u, 0u, 0u},
        {23u, 3u, 3u, 0u, 1u, 0u}
    };
    for (size_t i = 0; i < sizeof(corpus) / sizeof(corpus[0]); ++i) {
        (void)LLVMFuzzerTestOneInput(corpus[i], sizeof(corpus[i]));
    }
    return 0;
}
#endif
