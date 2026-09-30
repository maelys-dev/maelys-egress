#ifndef TEST_TLS_SOCKET_FIXTURE_H
#define TEST_TLS_SOCKET_FIXTURE_H

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", \
        __FILE__, __LINE__, #condition, errno); exit(1); } } while (0)

static inline void fixture_limits(void) {
    struct rlimit limit;
    REQUIRE(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    if (limit.rlim_cur > 4096) limit.rlim_cur = 4096;
    REQUIRE(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    alarm(60);
}

static inline int fixture_fd_count(void) {
    struct rlimit limit;
    REQUIRE(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    int count = 0;
    for (int fd = 0; fd < (int)limit.rlim_cur; ++fd)
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}

static inline void fixture_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    REQUIRE(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static inline void fixture_readable(int fd) {
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    REQUIRE(poll(&descriptor, 1, 2000) == 1);
    REQUIRE(descriptor.revents & (POLLIN | POLLHUP));
}

static inline void fixture_pair(int tcp, int pair[2]) {
    if (!tcp) REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    else {
        int listener = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listener >= 0);
        struct sockaddr_in address = {.sin_family = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        REQUIRE(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
        socklen_t length = sizeof(address);
        REQUIRE(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
        REQUIRE(listen(listener, 1) == 0);
        pair[0] = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(pair[0] >= 0);
        REQUIRE(connect(pair[0], (struct sockaddr *)&address, length) == 0);
        pair[1] = accept(listener, NULL, NULL);
        REQUIRE(pair[1] >= 0);
        close(listener);
    }
    fixture_nonblocking(pair[0]);
    fixture_nonblocking(pair[1]);
}

/* This hostile peer deliberately bypasses System. Repeat one valid original
 * descriptor: every received copy must be closed, while the original stays. */
static inline void fixture_rights(int socket_fd, const void *bytes,
                                 size_t length, size_t count) {
    REQUIRE(count > 0 && count <= 254);
    int original = open("/dev/null", O_RDONLY);
    REQUIRE(original >= 0);
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(254 * sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct iovec iov = {.iov_base = (void *)bytes, .iov_len = length};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = (socklen_t)CMSG_SPACE(count * sizeof(int))};
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = (socklen_t)CMSG_LEN(count * sizeof(int));
    for (size_t i = 0; i < count; ++i)
        memcpy((unsigned char *)CMSG_DATA(header) + i * sizeof(int), &original, sizeof(int));
    REQUIRE(sendmsg(socket_fd, &message, 0) == (ssize_t)length);
    REQUIRE(fcntl(original, F_GETFD) >= 0);
    close(original);
}

#endif
