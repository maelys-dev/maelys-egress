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

/* Test the exact production adapter with only its fdpass boundary replaced. */
#include "providers/socket_io.c"

int main(void) {
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
