#include "maelys/egress_tls_modules.h"
#include "src/internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MAELYS_TLS_FACTORY
#error MAELYS_TLS_FACTORY must name the provider creation function
#endif

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static int ready(maelys_egress_tls_step_t step) {
    return step == MAELYS_EGRESS_TLS_COMPLETE;
}

int main(void) {
    const char *certificate = getenv("MAELYS_TLS_TEST_CERT");
    const char *private_key = getenv("MAELYS_TLS_TEST_KEY");
    if (!certificate || !private_key) {
        fprintf(stderr, "TLS test identity environment is required\n");
        return 1;
    }
    maelys_egress_tls_files_t files = {
        .certificate_file = certificate,
        .private_key_file = private_key,
        .ca_file = certificate,
        .require_client_certificate = 1
    };
    maelys_egress_tls_provider_t *provider = NULL;
    char *error = NULL;
    CHECK(MAELYS_TLS_FACTORY(&files, &provider, &error) == MAELYS_EGRESS_OK);
    if (!provider) {
        fprintf(stderr, "provider creation: %s\n", error ? error : "no diagnostic");
        maelys_egress_error_free(error);
        return 1;
    }
    int sockets[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    for (size_t i = 0; i < 2u; ++i) {
        int flags = fcntl(sockets[i], F_GETFL, 0);
        CHECK(flags >= 0 && fcntl(sockets[i], F_SETFL, flags | O_NONBLOCK) == 0);
    }
    void *client = NULL;
    void *server = NULL;
    CHECK(provider->ops.session_create(provider->context, MAELYS_EGRESS_TLS_CLIENT,
        sockets[0], "localhost", &client, &error) == MAELYS_EGRESS_OK);
    CHECK(provider->ops.session_create(provider->context, MAELYS_EGRESS_TLS_SERVER,
        sockets[1], NULL, &server, &error) == MAELYS_EGRESS_OK);
    maelys_egress_tls_step_t client_step = MAELYS_EGRESS_TLS_WANT_WRITE;
    maelys_egress_tls_step_t server_step = MAELYS_EGRESS_TLS_WANT_READ;
    for (size_t i = 0; i < 10000u && (!ready(client_step) || !ready(server_step)); ++i) {
        if (!ready(client_step)) client_step = provider->ops.handshake(provider->context, client);
        if (!ready(server_step)) server_step = provider->ops.handshake(provider->context, server);
        CHECK(client_step != MAELYS_EGRESS_TLS_FAILED);
        CHECK(server_step != MAELYS_EGRESS_TLS_FAILED);
    }
    CHECK(ready(client_step) && ready(server_step));

    static const char message[] = "maelys authenticated TLS";
    size_t written = 0u;
    maelys_egress_tls_step_t step = provider->ops.write(provider->context, client,
        message, sizeof(message), &written);
    for (size_t i = 0; i < 10000u && step != MAELYS_EGRESS_TLS_COMPLETE; ++i) {
        CHECK(step != MAELYS_EGRESS_TLS_FAILED);
        step = provider->ops.write(provider->context, client,
            message, sizeof(message), &written);
    }
    CHECK(step == MAELYS_EGRESS_TLS_COMPLETE && written == sizeof(message));
    char received[sizeof(message)];
    size_t count = 0u;
    step = MAELYS_EGRESS_TLS_WANT_READ;
    for (size_t i = 0; i < 10000u && step != MAELYS_EGRESS_TLS_COMPLETE; ++i) {
        step = provider->ops.read(provider->context, server,
            received, sizeof(received), &count);
        CHECK(step != MAELYS_EGRESS_TLS_FAILED);
    }
    CHECK(step == MAELYS_EGRESS_TLS_COMPLETE && count == sizeof(message));
    CHECK(memcmp(received, message, sizeof(message)) == 0);

    provider->ops.session_release(provider->context, client);
    provider->ops.session_release(provider->context, server);
    (void)close(sockets[0]);
    (void)close(sockets[1]);
    maelys_egress_tls_provider_release(provider);
    maelys_egress_error_free(error);
    if (failures) return 1;
    puts("TLS provider tests passed");
    return 0;
}
