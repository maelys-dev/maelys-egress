#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "maelys/sys/fdpass.h"
#include "tests/tls_socket_fixture.h"

/* Deterministic error/control outcomes that are not all reproducible on
 * every kernel. The real-kernel rights/closure cases live in test_tls_socket. */
static maelys_sys_result_t next_result;
static unsigned next_flags;
static size_t next_bytes;
static int next_errno;
static int calls;
static sa_family_t advertised_family;
static int address_error;

/* Keep real stream I/O while simulating a family whose usable endpoints
 * require a VM, such as AF_VSOCK. No VM setup is needed in the default gate. */
static int test_getsockname(int fd, struct sockaddr *address, socklen_t *length) {
    if (address_error) { errno = EIO; return -1; }
    int result = getsockname(fd, address, length);
    if (result == 0 && advertised_family != AF_UNSPEC)
        address->sa_family = advertised_family;
    return result;
}

maelys_sys_result_t maelys_sys_fd_stream_receive(int fd, void *buffer,
    size_t capacity, size_t *out_received, int *out_fds, size_t fd_capacity,
    size_t *out_fd_count, unsigned *out_flags) {
    REQUIRE(fd == 42 && buffer && capacity > 0);
    REQUIRE(out_fds == NULL && fd_capacity == 0);
    ++calls;
    *out_received = next_bytes;
    *out_fd_count = 0;
    *out_flags = next_flags;
    errno = next_errno;
    return next_result;
}

/* Test the production adapter with controlled fdpass outcomes and family
 * discovery; fcntl, SO_TYPE and byte-stream reads still reach the kernel. */
#define getsockname test_getsockname
#include "providers/socket_io.c"
#undef getsockname

static void other_stream_family(void) {
    int pair[2];
    fixture_pair(1, pair);
    int original_flags = fcntl(pair[1], F_GETFL);
    advertised_family = AF_VSOCK;
    egress_tls_socket_t transport;
    REQUIRE(egress_tls_socket_init(&transport, pair[1]) == 0);
    REQUIRE(!transport.unix_stream && !transport.failed);
    char bytes[8];
    int before = calls;
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == -1);
    REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);
    REQUIRE(send(pair[0], "x", 1, 0) == 1);
    fixture_readable(pair[1]);
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == 1 && bytes[0] == 'x');
    REQUIRE(shutdown(pair[0], SHUT_WR) == 0);
    fixture_readable(pair[1]);
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == 0);
    REQUIRE(calls == before && fcntl(pair[1], F_GETFL) == original_flags);

    /* A failed family lookup must never silently select the byte-only path. */
    address_error = 1;
    REQUIRE(egress_tls_socket_init(&transport, pair[1]) == -1);
    REQUIRE(transport.failed && transport.fd == -1);
    address_error = 0;
    advertised_family = AF_UNSPEC;
    close(pair[0]); close(pair[1]);
}

int main(void) {
    fixture_limits();
    other_stream_family();
    egress_tls_socket_t transport = {.fd = 42, .unix_stream = 1};
    char bytes[8];
    next_result = MAELYS_SYS_OK;
    next_bytes = 1;
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == 1);
    const unsigned flags[] = {MAELYS_SYS_FDPASS_TRUNCATED,
        MAELYS_SYS_FDPASS_CONTROL_TRUNCATED, MAELYS_SYS_FDPASS_SURPLUS,
        MAELYS_SYS_FDPASS_UNEXPECTED_CONTROL};
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        transport.failed = 0;
        next_flags = flags[i];
        REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == -1 && errno == EPROTO);
        REQUIRE(transport.failed);
        int before = calls;
        next_flags = 0;
        REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == -1 && errno == EPROTO);
        REQUIRE(calls == before);
    }
    transport.failed = 0;
    next_bytes = 0; /* control-only OK must never become EOF or WANT_READ */
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == -1 && errno == EPROTO);
    REQUIRE(transport.failed);
    transport.failed = 0;
    next_result = MAELYS_SYS_ERR_WOULD_BLOCK;
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == -1 && errno == EAGAIN);
    REQUIRE(!transport.failed);
    next_result = MAELYS_SYS_ERR_OS;
    next_errno = EINTR;
    int before = calls;
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == -1 && errno == EINTR);
    REQUIRE(calls == before + 1 && !transport.failed); /* no internal spin */
    next_errno = EMFILE;
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == -1 && errno == EMFILE);
    next_result = MAELYS_SYS_ERR_RESET;
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == -1 && errno == ECONNRESET);
    next_result = MAELYS_SYS_ERR_CLOSED;
    REQUIRE(egress_tls_socket_receive(&transport, bytes, sizeof(bytes)) == 0);
    puts("TLS socket error tests passed");
    return 0;
}
