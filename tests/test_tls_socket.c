#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "providers/socket_io.h"
#include "tests/tls_socket_fixture.h"

static void ordinary(int tcp) {
    int pair[2];
    fixture_pair(tcp, pair);
    egress_tls_socket_t transport;
    int original_flags = fcntl(pair[1], F_GETFL);
    REQUIRE(egress_tls_socket_init(&transport, pair[1]) == 0);
    REQUIRE(transport.unix_stream == !tcp);
    char bytes[8];
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == -1);
    REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);
    REQUIRE(send(pair[0], "abc", 3, 0) == 3);
    fixture_readable(pair[1]);
    REQUIRE(egress_tls_socket_receive(&transport, bytes, 1) == 1 && bytes[0] == 'a');
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == 2);
    REQUIRE(memcmp(bytes, "bc", 2) == 0);
    REQUIRE(shutdown(pair[0], SHUT_WR) == 0);
    fixture_readable(pair[1]);
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == 0);
    REQUIRE(fcntl(pair[1], F_GETFL) == original_flags);
    close(pair[0]); close(pair[1]);
}

static void rights(size_t count, size_t length) {
    int pair[2];
    fixture_pair(0, pair);
    egress_tls_socket_t transport;
    REQUIRE(egress_tls_socket_init(&transport, pair[1]) == 0);
    int before = fixture_fd_count();
    fixture_rights(pair[0], "abc", length, count);
    char byte;
    REQUIRE(egress_tls_socket_receive(&transport, &byte, 1) == -1);
    REQUIRE(errno == EPROTO && transport.failed);
    REQUIRE(fixture_fd_count() == before);
    /* A failed TLS transport never resumes, even after an empty rights
     * message or when retrying after the poisoned byte has been consumed. */
    REQUIRE(send(pair[0], "x", 1, 0) == 1);
    REQUIRE(egress_tls_socket_receive(&transport, &byte, 1) == -1 && errno == EPROTO);
    close(pair[0]); close(pair[1]);
}

static void repeated_attacks(void) {
    int baseline = fixture_fd_count();
    for (size_t attempt = 0; attempt < 128; ++attempt) {
        int pair[2];
        fixture_pair(0, pair);
        egress_tls_socket_t transport;
        REQUIRE(egress_tls_socket_init(&transport, pair[1]) == 0);
        /* The second transfer is still queued when the first one poisons
         * the session. Closing the borrowed socket must clean that up too. */
        fixture_rights(pair[0], "a", 1, 50);
        fixture_rights(pair[0], "b", 1, 50);
        char byte;
        REQUIRE(egress_tls_socket_receive(&transport, &byte, 1) == -1 && errno == EPROTO);
        REQUIRE(fixture_fd_count() == baseline + 2);
        close(pair[0]); close(pair[1]);
        REQUIRE(fixture_fd_count() == baseline);
    }
}

#ifdef __linux__
static void credentials(void) {
    for (int eof = 0; eof < 2; ++eof) {
        int pair[2];
        fixture_pair(0, pair);
        int enabled = 1;
        REQUIRE(setsockopt(pair[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
        egress_tls_socket_t transport;
        REQUIRE(egress_tls_socket_init(&transport, pair[1]) == 0);
        if (eof) REQUIRE(shutdown(pair[0], SHUT_WR) == 0);
        else REQUIRE(send(pair[0], "x", 1, 0) == 1);
        char byte;
        ssize_t count = egress_tls_socket_receive(&transport, &byte, 1);
        if (eof) REQUIRE(count == 0 && !transport.failed);
        else REQUIRE(count == -1 && errno == EPROTO && transport.failed);
        close(pair[0]); close(pair[1]);
    }
}
#endif

int main(void) {
    fixture_limits();
    int baseline = fixture_fd_count();
    int pair[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    egress_tls_socket_t transport;
    REQUIRE(egress_tls_socket_init(&transport, pair[1]) == -1);
    REQUIRE(!(fcntl(pair[1], F_GETFL) & O_NONBLOCK));
    close(pair[0]); close(pair[1]);
    REQUIRE(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    fixture_nonblocking(pair[1]);
    REQUIRE(egress_tls_socket_init(&transport, pair[1]) == -1);
    close(pair[0]); close(pair[1]);
    REQUIRE(egress_tls_socket_init(&transport, -1) == -1);
    ordinary(0);
    ordinary(1);
    rights(1, 3);
    rights(50, 3);
    repeated_attacks();
#ifdef __APPLE__
    rights(254, 3);
    rights(50, 0);
#else
    rights(253, 3);
    credentials();
#endif
    REQUIRE(fixture_fd_count() == baseline);
    puts("TLS socket tests passed");
    return 0;
}
