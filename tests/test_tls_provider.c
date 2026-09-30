#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "maelys/egress_tls_modules.h"
#include "src/internal.h"
#include "tests/tls_socket_fixture.h"

#include <poll.h>

#ifndef MAELYS_TLS_FACTORY
#error MAELYS_TLS_FACTORY must name the provider creation function
#endif

typedef struct tls_pair {
    maelys_egress_tls_provider_t *provider;
    void *session[2]; /* client, server */
    int fd[2];
    int relay[2];
} tls_pair_t;

static void pair_create(tls_pair_t *pair, maelys_egress_tls_provider_t *provider, int tcp) {
    *pair = (tls_pair_t){.provider = provider, .relay = {-1, -1}};
    if (tcp) fixture_pair(1, pair->fd);
    else {
        for (size_t i = 0; i < 2; ++i) {
            int sockets[2];
            fixture_pair(0, sockets);
            pair->fd[i] = sockets[0];
            pair->relay[i] = sockets[1];
        }
    }
    char *error = NULL;
    REQUIRE(provider->ops.session_create(provider->context, MAELYS_EGRESS_TLS_CLIENT,
        pair->fd[0], "localhost", &pair->session[0], &error) == MAELYS_EGRESS_OK);
    REQUIRE(provider->ops.session_create(provider->context, MAELYS_EGRESS_TLS_SERVER,
        pair->fd[1], NULL, &pair->session[1], &error) == MAELYS_EGRESS_OK);
    maelys_egress_error_free(error);
}

static void pair_release(tls_pair_t *pair) {
    for (size_t i = 0; i < 2; ++i) {
        int flags = fcntl(pair->fd[i], F_GETFL);
        pair->provider->ops.session_release(pair->provider->context, pair->session[i]);
        /* TLS borrows the socket: release neither closes nor changes it. */
        REQUIRE(fcntl(pair->fd[i], F_GETFL) == flags);
        close(pair->fd[i]);
        if (pair->relay[i] >= 0) close(pair->relay[i]);
    }
}

static void send_bytes(int fd, const unsigned char *bytes, size_t length) {
    while (length) {
        ssize_t count = send(fd, bytes, length, 0);
        REQUIRE(count > 0);
        bytes += count;
        length -= (size_t)count;
    }
}

/* Relay ciphertext without decoding it. All malicious ancillary data goes
 * toward the provider, never toward the test's plain recv calls. */
static void pump(tls_pair_t *pair) {
    if (pair->relay[0] < 0) return;
    for (size_t i = 0; i < 2; ++i) {
        unsigned char bytes[4096];
        ssize_t count;
        while ((count = recv(pair->relay[i], bytes, sizeof(bytes), 0)) > 0)
            send_bytes(pair->relay[1 - i], bytes, (size_t)count);
        REQUIRE(count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }
}

static void handshake(tls_pair_t *pair) {
    maelys_egress_tls_step_t steps[2] = {
        MAELYS_EGRESS_TLS_WANT_READ, MAELYS_EGRESS_TLS_WANT_READ};
    for (size_t attempt = 0; attempt < 2000; ++attempt) {
        for (size_t i = 0; i < 2; ++i) {
            if (steps[i] != MAELYS_EGRESS_TLS_COMPLETE)
                steps[i] = pair->provider->ops.handshake(pair->provider->context, pair->session[i]);
            REQUIRE(steps[i] != MAELYS_EGRESS_TLS_FAILED);
            pump(pair);
        }
        if (steps[0] == MAELYS_EGRESS_TLS_COMPLETE && steps[1] == MAELYS_EGRESS_TLS_COMPLETE) return;
        (void)poll(NULL, 0, 1);
    }
    REQUIRE(0 && "handshake deadline");
}

static const char plaintext[] = "maelys authenticated TLS";

static void write_record(tls_pair_t *pair, size_t sender) {
    size_t written = 0;
    REQUIRE(pair->provider->ops.write(pair->provider->context, pair->session[sender],
        plaintext, sizeof(plaintext), &written) == MAELYS_EGRESS_TLS_COMPLETE);
    REQUIRE(written == sizeof(plaintext));
}

static void read_record(tls_pair_t *pair, size_t receiver) {
    char bytes[sizeof(plaintext)];
    size_t received = 0;
    for (size_t attempt = 0; attempt < 2000; ++attempt) {
        size_t count = 0;
        maelys_egress_tls_step_t step = pair->provider->ops.read(pair->provider->context,
            pair->session[receiver], bytes + received, sizeof(bytes) - received, &count);
        REQUIRE(step == MAELYS_EGRESS_TLS_COMPLETE || step == MAELYS_EGRESS_TLS_WANT_READ ||
            step == MAELYS_EGRESS_TLS_WANT_WRITE);
        received += count;
        if (received == sizeof(bytes)) {
            REQUIRE(memcmp(bytes, plaintext, sizeof(bytes)) == 0);
            return;
        }
        (void)poll(NULL, 0, 1);
    }
    REQUIRE(0 && "read deadline");
}

static void ordinary(maelys_egress_tls_provider_t *provider, int tcp) {
    tls_pair_t pair;
    pair_create(&pair, provider, tcp);
    handshake(&pair);
    for (size_t sender = 0; sender < 2; ++sender) {
        write_record(&pair, sender);
        pump(&pair);
        read_record(&pair, 1 - sender);
    }
    pair_release(&pair);
}

static void assert_failed(tls_pair_t *pair, size_t receiver, int established) {
    void *session = pair->session[receiver];
    maelys_egress_tls_provider_t *provider = pair->provider;
    char bytes[128];
    size_t count = 99;
    maelys_egress_tls_step_t step = established
        ? provider->ops.read(provider->context, session, bytes, sizeof(bytes), &count)
        : provider->ops.handshake(provider->context, session);
    REQUIRE(step == MAELYS_EGRESS_TLS_FAILED);
    if (established) REQUIRE(count == 0);
    REQUIRE(provider->ops.last_error(provider->context, session) != NULL);
    /* A retry cannot resurrect a poisoned session, drain buffered plaintext
     * or emit more ciphertext after the violation. */
    REQUIRE(provider->ops.handshake(provider->context, session) == MAELYS_EGRESS_TLS_FAILED);
    REQUIRE(provider->ops.read(provider->context, session, bytes, sizeof(bytes), &count) == MAELYS_EGRESS_TLS_FAILED);
    REQUIRE(count == 0);
    REQUIRE(provider->ops.write(provider->context, session, plaintext, sizeof(plaintext), &count) == MAELYS_EGRESS_TLS_FAILED);
    REQUIRE(count == 0);
    REQUIRE(provider->ops.shutdown(provider->context, session) == MAELYS_EGRESS_TLS_FAILED);
}

/* Feed a genuine ClientHello/server flight/application record with attached
 * rights: refusing invalid TLS syntax alone cannot make this test pass. */
static void attack(maelys_egress_tls_provider_t *provider, size_t receiver,
                   int established, int control_only) {
    tls_pair_t pair;
    pair_create(&pair, provider, 0);
    if (established) {
        handshake(&pair);
        /* Consume post-handshake tickets before testing application data. */
        for (size_t sender = 0; sender < 2; ++sender) {
            write_record(&pair, sender); pump(&pair); read_record(&pair, 1 - sender);
        }
        write_record(&pair, 1 - receiver);
    } else {
        REQUIRE(provider->ops.handshake(provider->context, pair.session[0]) != MAELYS_EGRESS_TLS_FAILED);
        if (receiver == 0) {
            pump(&pair);
            REQUIRE(provider->ops.handshake(provider->context, pair.session[1]) != MAELYS_EGRESS_TLS_FAILED);
        }
    }
    unsigned char ciphertext[16384];
    ssize_t length = recv(pair.relay[1 - receiver], ciphertext, sizeof(ciphertext), 0);
    REQUIRE(length > 1);
    int baseline = fixture_fd_count();
    if (control_only) fixture_rights(pair.relay[receiver], ciphertext, 0, 50);
    else {
        /* Fragment the TLS header before the rights-bearing byte. */
        send_bytes(pair.relay[receiver], ciphertext, 1);
        fixture_rights(pair.relay[receiver], ciphertext + 1, (size_t)length - 1, 50);
    }
    assert_failed(&pair, receiver, established);
    REQUIRE(fixture_fd_count() == baseline);
    pair_release(&pair);
}

int main(void) {
    fixture_limits();
    const char *certificate = getenv("MAELYS_TLS_TEST_CERT");
    const char *private_key = getenv("MAELYS_TLS_TEST_KEY");
    REQUIRE(certificate && private_key);
    maelys_egress_tls_files_t files = {
        .abi_version = MAELYS_EGRESS_TLS_FILES_ABI_VERSION,
        .certificate_file = certificate, .private_key_file = private_key,
        .ca_file = certificate, .require_client_certificate = 1};
    maelys_egress_tls_provider_t *provider = NULL;
    char *error = NULL;
    files.abi_version++;
    REQUIRE(MAELYS_TLS_FACTORY(&files, &provider, &error) == MAELYS_EGRESS_ERR_UNSUPPORTED);
    REQUIRE(provider == NULL && error && strstr(error, "ABI 2") && strstr(error, "ABI 1"));
    maelys_egress_error_free(error);
    error = NULL;
    files.abi_version = MAELYS_EGRESS_TLS_FILES_ABI_VERSION;
    REQUIRE(MAELYS_TLS_FACTORY(&files, &provider, &error) == MAELYS_EGRESS_OK && provider);
    int baseline = fixture_fd_count();
    ordinary(provider, 0);
    ordinary(provider, 1);
    for (size_t receiver = 0; receiver < 2; ++receiver) {
        for (int established = 0; established < 2; ++established) {
            attack(provider, receiver, established, 0);
#ifdef __APPLE__
            attack(provider, receiver, established, 1);
#endif
        }
    }
    REQUIRE(fixture_fd_count() == baseline);
    maelys_egress_tls_provider_release(provider);
    maelys_egress_error_free(error);
    puts("TLS provider tests passed (Unix/TCP, client/server, handshake/data, rights)");
    return 0;
}
