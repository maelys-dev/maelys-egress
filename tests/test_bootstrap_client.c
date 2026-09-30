/* SPDX-License-Identifier: MPL-2.0 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "maelys/egress_client.h"
#include "maelys/egress_channel.h"
#include "maelys/sys/fdpass.h"
#include "common/bootstrap.h"
#include "tests/tls_socket_fixture.h"
#include <signal.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>

static void codec_tests(void) {
    unsigned char request[8];
    egress_bootstrap_request(request);
    REQUIRE(!memcmp(request, "MEBQ\1\1\0\0", 8u));
    REQUIRE(egress_bootstrap_decode_request(request, 8u) == EGRESS_BOOTSTRAP_OK);
    for (size_t i = 0u; i < 8u; ++i) {
        unsigned char broken[8]; memcpy(broken, request, 8u); broken[i] ^= 3u;
        REQUIRE(egress_bootstrap_decode_request(broken, 8u) ==
            (i == 4u || i == 5u ? EGRESS_BOOTSTRAP_UNSUPPORTED : EGRESS_BOOTSTRAP_MALFORMED));
        REQUIRE(egress_bootstrap_decode_request(request, i) == EGRESS_BOOTSTRAP_MALFORMED);
    }
    for (unsigned code = 0u; code < 9u; ++code) {
        unsigned char response[16];
        egress_bootstrap_response(code, UINT64_C(0x0102030405060708), response);
        unsigned status; uint64_t timeout;
        REQUIRE(egress_bootstrap_decode_response(response, 16u, &status, &timeout));
        REQUIRE(status == (code > 7u ? EGRESS_BOOTSTRAP_INTERNAL : code));
        REQUIRE(timeout == (code ? 0u : UINT64_C(0x0102030405060708)));
        if (!code) REQUIRE(!memcmp(response + 8, "\1\2\3\4\5\6\7\10", 8u));
        for (size_t i = 0; i < 16u; ++i)
            REQUIRE(!egress_bootstrap_decode_response(response, i, &status, &timeout));
    }
    unsigned char response[16]; unsigned status; uint64_t timeout;
    egress_bootstrap_response(EGRESS_BOOTSTRAP_BUSY, 0u, response);
    REQUIRE(response[5] == MAELYS_EGRESS_CHANNEL_TIMEOUT);
    memcpy(response, "MECP", 4u);
    REQUIRE(!egress_bootstrap_decode_response(response, 16u, &status, &timeout));
    egress_bootstrap_response(EGRESS_BOOTSTRAP_OK, 0u, response);
    REQUIRE(!egress_bootstrap_decode_response(response, 16u, &status, &timeout));
    egress_bootstrap_response(EGRESS_BOOTSTRAP_OK, 100u, response);
    for (size_t i = 0; i < 8u; ++i) {
        if (i == 5u) continue;
        response[i] ^= 2u;
        REQUIRE(!egress_bootstrap_decode_response(response, 16u, &status, &timeout));
        response[i] ^= 2u;
    }
    const char *bad[] = {"relative", "/", "/x/", "/x//y", "/x/./y", "/x/../y"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i)
        REQUIRE(!egress_bootstrap_path_valid(bad[i]));
}

static volatile sig_atomic_t signals;
static void interrupted(int signum) { (void)signum; ++signals; }
static uint64_t milliseconds(void) {
    struct timespec now; REQUIRE(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void multiple_channels(int peer, const void *bytes, size_t length, int fd) {
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(50u * sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct iovec iov = {.iov_base = (void *)bytes, .iov_len = length};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1u,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(50u * sizeof(int));
    for (size_t i = 0u; i < 50u; ++i)
        memcpy((unsigned char *)CMSG_DATA(header) + i * sizeof(int), &fd, sizeof(int));
    REQUIRE(sendmsg(peer, &message, 0) == (ssize_t)length);
}

/* A separate malicious process cannot hide leaked descriptors in its own
 * count. The client binary links only its standalone archive, not the core. */
static void exchange(unsigned status, int attack, size_t split,
    maelys_egress_client_result_t expected) {
    char template[] = "/tmp/egress-bootstrap-client-XXXXXX";
    REQUIRE(mkdtemp(template));
    char *directory = realpath(template, NULL); REQUIRE(directory);
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    REQUIRE(snprintf(path, sizeof(path), "%s/s", directory) > 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX}; strcpy(address.sun_path, path);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0); REQUIRE(listener >= 0);
    REQUIRE(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    REQUIRE(listen(listener, 4) == 0);
    int before = fixture_fd_count();
    pid_t child = fork(); REQUIRE(child >= 0);
    if (!child) {
        alarm(5); (void)signal(SIGPIPE, SIG_IGN);
        int peer = accept(listener, NULL, NULL); REQUIRE(peer >= 0);
        unsigned char request[8]; size_t used = 0u;
        while (used < 8u) {
            ssize_t n = recv(peer, request + used, 8u - used, 0);
            REQUIRE(n > 0); used += (size_t)n;
        }
        REQUIRE(egress_bootstrap_decode_request(request, 8u) == EGRESS_BOOTSTRAP_OK);
        if (attack == 7) { /* Slow reply plus repeated EINTR in the client. */
            struct timespec delay = {.tv_nsec = 200000000L}; nanosleep(&delay, NULL);
            close(peer); _exit(0);
        }
        unsigned char response[16]; egress_bootstrap_response(status, 5000u, response);
        if (attack == 5) response[7] = 1u;
        if (attack == 6) memcpy(response, "MECP", 4u);
        int pair[2]; REQUIRE(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
        fixture_nonblocking(peer);
        size_t sent = 0u;
        if (attack == 4) multiple_channels(peer, response, 16u, pair[0]);
        else if (attack == 3 || attack == 8) {
            fixture_rights(peer, response, attack == 8 ? 0u : 16u, 1u);
        } else if (attack == 9) { /* Rights on a later byte are not acceptable. */
            REQUIRE(send(peer, response, 1u, 0) == 1);
            REQUIRE(maelys_sys_fd_stream_send(peer, response + 1u, 15u, pair[0], &sent) == MAELYS_SYS_OK);
        } else {
            int passed = (status == 0u && attack != 1) || attack == 2 ? pair[0] : -1;
            REQUIRE(maelys_sys_fd_stream_send(peer, response, split, passed, &sent) == MAELYS_SYS_OK);
            REQUIRE(sent == split);
            if (attack == 10) { close(peer); _exit(0); }
            for (size_t i = split; i < 16u; ++i) {
                REQUIRE(maelys_sys_fd_stream_send(peer, response + i, 1u, -1, &sent) == MAELYS_SYS_OK);
                REQUIRE(sent == 1u);
            }
        }
        /* Keep the lease alive until the real client closes it. */
        struct pollfd p = {.fd = peer, .events = POLLIN};
        REQUIRE(poll(&p, 1u, 2000) == 1);
        close(pair[0]); close(pair[1]); close(peer); close(listener); _exit(0);
    }
    struct itimerval timer = {0};
    if (attack == 7) {
        struct sigaction action = {.sa_handler = interrupted};
        REQUIRE(sigaction(SIGALRM, &action, NULL) == 0);
        timer.it_interval.tv_usec = 2000; timer.it_value = timer.it_interval;
        REQUIRE(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    }
    uint64_t began = milliseconds();
    maelys_egress_client_channel_t *channel = NULL; char *error = NULL;
    maelys_egress_client_result_t got = maelys_egress_client_channel_open(path,
        attack == 7 ? 80u : 1000u, &channel, &error);
    if (attack == 7) {
        memset(&timer, 0, sizeof(timer)); REQUIRE(setitimer(ITIMER_REAL, &timer, NULL) == 0);
        REQUIRE(signals > 0 && milliseconds() - began < 180u);
        alarm(60);
    }
    REQUIRE(got == expected);
    if (got == MAELYS_EGRESS_CLIENT_OK) {
        REQUIRE(channel && !error);
        REQUIRE(maelys_egress_client_channel_connect_timeout_ms(channel) == 5000u);
        REQUIRE(fcntl(maelys_egress_client_channel_fd(channel), F_GETFD) & FD_CLOEXEC);
        REQUIRE(fixture_fd_count() == before + 2);
    } else REQUIRE(!channel && error);
    maelys_egress_client_error_free(error);
    maelys_egress_client_channel_close(channel);
    REQUIRE(fixture_fd_count() == before);
    int outcome; REQUIRE(waitpid(child, &outcome, 0) == child);
    REQUIRE(WIFEXITED(outcome) && WEXITSTATUS(outcome) == 0);
    close(listener); REQUIRE(unlink(path) == 0); REQUIRE(rmdir(directory) == 0); free(directory);
}

int main(void) {
    fixture_limits(); codec_tests();
    REQUIRE(maelys_egress_client_channel_fd(NULL) == -1);
    REQUIRE(maelys_egress_client_channel_connect_timeout_ms(NULL) == 0u);
    maelys_egress_client_channel_close(NULL);
    for (size_t split = 1u; split <= 16u; ++split) exchange(0u, 0, split, MAELYS_EGRESS_CLIENT_OK);
    const maelys_egress_client_result_t results[] = {
        MAELYS_EGRESS_CLIENT_OK, MAELYS_EGRESS_CLIENT_ERR_DENIED, MAELYS_EGRESS_CLIENT_ERR_BUSY,
        MAELYS_EGRESS_CLIENT_ERR_CANCELLED, MAELYS_EGRESS_CLIENT_ERR_PROTOCOL,
        MAELYS_EGRESS_CLIENT_ERR_UNSUPPORTED, MAELYS_EGRESS_CLIENT_ERR_RESOURCE,
        MAELYS_EGRESS_CLIENT_ERR_INTERNAL, MAELYS_EGRESS_CLIENT_ERR_INTERNAL};
    for (unsigned status = 1u; status < 9u; ++status) {
        exchange(status, 0, 1u, results[status]);
        exchange(status, 2, 16u, MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    }
    for (int attack = 1; attack <= 6; ++attack) {
        if (attack == 2) continue;
        exchange(0u, attack, 1u, MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    }
    exchange(0u, 7, 1u, MAELYS_EGRESS_CLIENT_ERR_UNANSWERED);
#ifdef __APPLE__
    exchange(0u, 8, 1u, MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
#endif
    exchange(0u, 9, 1u, MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    exchange(0u, 10, 1u, MAELYS_EGRESS_CLIENT_ERR_IO);
    puts("bootstrap client: codec, fragments, ancillary attacks, deadline and ownership passed");
    return 0;
}
