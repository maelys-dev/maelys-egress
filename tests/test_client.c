/*
 * The channel client against a server written in this file: every branch
 * of the contract the client must hold — one descriptor on OK and none
 * otherwise, versions, truncation, the read deadline that shuts the channel,
 * and what it leaves open. The binary links libmaelys_egress_client alone,
 * without maelys-system and without -pthread: that link is the proof the
 * archive stands on the C library.
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "maelys/egress_client.h"
#include "maelys/egress_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "client: %s failed at line %d\n", #condition, __LINE__); \
            exit(1); \
        } \
    } while (0)

/* The number of open descriptors below a small bound: enough to see a leak
 * of one descriptor in a test process. */
static int open_descriptors(void) {
    int count = 0;
    for (int fd = 0; fd < 64; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    }
    return count;
}

static void channel_pair(int pair[2]) {
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
}

/* Sends response bytes with the given descriptors attached; a count of 0
 * sends no ancillary data. */
static void server_send(int server_fd, const unsigned char *bytes, size_t length,
                        const int *descriptors, size_t count) {
    struct iovec iov = {.iov_base = (void *)bytes, .iov_len = length};
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int) * 32u)];
    } control;
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1u;
    CHECK(count <= 32u);
    if (count) {
        memset(&control, 0, sizeof(control));
        message.msg_control = control.bytes;
        message.msg_controllen = (socklen_t)CMSG_SPACE(sizeof(int) * count);
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = (socklen_t)CMSG_LEN(sizeof(int) * count);
        memcpy(CMSG_DATA(header), descriptors, sizeof(int) * count);
    }
    CHECK(sendmsg(server_fd, &message, 0) == (ssize_t)length);
}

static void server_expect_request(int server_fd, const char *host, uint16_t port) {
    unsigned char request[MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE + 1u];
    ssize_t received = recv(server_fd, request, sizeof(request), 0);
    CHECK(received > 0);
    maelys_egress_channel_request_t decoded;
    CHECK(maelys_egress_channel_decode_request(request, (size_t)received, &decoded, NULL) ==
          MAELYS_EGRESS_CHANNEL_OK);
    CHECK(strcmp(decoded.host, host) == 0 && decoded.port == port);
}

static void respond(int server_fd, maelys_egress_channel_status_t status,
                    const int *descriptors, size_t count) {
    unsigned char response[MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE];
    CHECK(maelys_egress_channel_encode_response(status, response, sizeof(response)) ==
          sizeof(response));
    server_send(server_fd, response, sizeof(response), descriptors, count);
}

static void test_success_hands_over_one_stream(void) {
    int channel[2];
    int stream[2];
    channel_pair(channel);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, stream) == 0);
    int before = open_descriptors();
    int received = -1;
    char *error = NULL;
    /* The request waits in the socket pair; the answer is queued ahead. */
    respond(channel[1], MAELYS_EGRESS_CHANNEL_OK, &stream[1], 1u);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 1000u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_OK);
    CHECK(error == NULL && received >= 0);
    server_expect_request(channel[1], "example.com", 443u);
    CHECK(fcntl(received, F_GETFD) & FD_CLOEXEC);
    CHECK(send(stream[0], "relayed", 7u, 0) == 7);
    char bytes[8] = {0};
    CHECK(recv(received, bytes, sizeof(bytes), 0) == 7 && memcmp(bytes, "relayed", 7u) == 0);
    (void)close(received);
    /* One descriptor came in, one was handed over: nothing else is open. */
    CHECK(open_descriptors() == before);
    (void)close(channel[0]);
    (void)close(channel[1]);
    (void)close(stream[0]);
    (void)close(stream[1]);
}

static void test_refusals_carry_no_stream(void) {
    static const struct {
        maelys_egress_channel_status_t status;
        maelys_egress_client_result_t result;
    } cases[] = {
        {MAELYS_EGRESS_CHANNEL_DENIED, MAELYS_EGRESS_CLIENT_ERR_DENIED},
        {MAELYS_EGRESS_CHANNEL_TIMEOUT, MAELYS_EGRESS_CLIENT_ERR_TIMEOUT},
        {MAELYS_EGRESS_CHANNEL_CANCELLED, MAELYS_EGRESS_CLIENT_ERR_CANCELLED},
        {MAELYS_EGRESS_CHANNEL_MALFORMED, MAELYS_EGRESS_CLIENT_ERR_PROTOCOL},
        {MAELYS_EGRESS_CHANNEL_UNSUPPORTED, MAELYS_EGRESS_CLIENT_ERR_UNSUPPORTED},
        {MAELYS_EGRESS_CHANNEL_RESOURCE, MAELYS_EGRESS_CLIENT_ERR_RESOURCE},
        {MAELYS_EGRESS_CHANNEL_INTERNAL, MAELYS_EGRESS_CLIENT_ERR_INTERNAL},
        {(maelys_egress_channel_status_t)99, MAELYS_EGRESS_CLIENT_ERR_INTERNAL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int channel[2];
        channel_pair(channel);
        int before = open_descriptors();
        respond(channel[1], cases[i].status, NULL, 0u);
        int received = 7;
        char *error = NULL;
        CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 1000u, &received,
                                           &error) == cases[i].result);
        CHECK(received == -1 && error != NULL);
        maelys_egress_client_error_free(error);
        CHECK(open_descriptors() == before);
        (void)close(channel[0]);
        (void)close(channel[1]);
    }
}

static void test_descriptor_cardinality(void) {
    int channel[2];
    int extra[2];
    channel_pair(channel);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, extra) == 0);
    int before = open_descriptors();
    int received = -1;
    char *error = NULL;
    /* Two descriptors on OK: both closed, protocol error. */
    respond(channel[1], MAELYS_EGRESS_CHANNEL_OK, extra, 2u);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 1000u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    CHECK(received == -1 && error && strstr(error, "more than one descriptor"));
    maelys_egress_client_error_free(error);
    CHECK(open_descriptors() == before);
    /* Twenty on OK, more than the client once had room for: every one is
     * closed, none is read past the control bytes the kernel filled. */
    int twenty[20];
    for (size_t i = 0; i < 20u; ++i) twenty[i] = extra[0];
    respond(channel[1], MAELYS_EGRESS_CHANNEL_OK, twenty, 20u);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 1000u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    CHECK(received == -1 && error && strstr(error, "more than one descriptor"));
    maelys_egress_client_error_free(error);
    CHECK(open_descriptors() == before);
    /* No descriptor on OK. */
    respond(channel[1], MAELYS_EGRESS_CHANNEL_OK, NULL, 0u);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 1000u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    CHECK(received == -1 && error && strstr(error, "no descriptor"));
    maelys_egress_client_error_free(error);
    /* A descriptor on DENIED: closed, and malformed rather than denied. */
    respond(channel[1], MAELYS_EGRESS_CHANNEL_DENIED, &extra[0], 1u);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 1000u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    CHECK(received == -1 && error && strstr(error, "carried a descriptor"));
    maelys_egress_client_error_free(error);
    CHECK(open_descriptors() == before);
    (void)close(channel[0]);
    (void)close(channel[1]);
    (void)close(extra[0]);
    (void)close(extra[1]);
}

static void test_malformed_responses(void) {
    int channel[2];
    channel_pair(channel);
    int received = -1;
    char *error = NULL;
    static const unsigned char short_response[7] = {0x4d, 0x45, 0x43, 0x50, 1u, 0u, 0u};
    server_send(channel[1], short_response, sizeof(short_response), NULL, 0u);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 1000u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    CHECK(error && strstr(error, "malformed"));
    maelys_egress_client_error_free(error);
    static const unsigned char long_response[9] = {0x4d, 0x45, 0x43, 0x50, 1u, 0u, 0u, 0u, 0u};
    server_send(channel[1], long_response, sizeof(long_response), NULL, 0u);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 1000u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    maelys_egress_client_error_free(error);
    static const unsigned char old_magic[8] = {0x4d, 0x45, 0x58, 0x52, 1u, 0u, 0u, 0u};
    server_send(channel[1], old_magic, sizeof(old_magic), NULL, 0u);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 1000u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    maelys_egress_client_error_free(error);
    /* Version 2 with OK: not ours, malformed. Version 2 with UNSUPPORTED:
     * the server telling us which version it speaks. */
    static const unsigned char version_2_ok[8] = {0x4d, 0x45, 0x43, 0x50, 2u, 0u, 0u, 0u};
    server_send(channel[1], version_2_ok, sizeof(version_2_ok), NULL, 0u);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 1000u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_PROTOCOL);
    CHECK(error && strstr(error, "version 2"));
    maelys_egress_client_error_free(error);
    static const unsigned char version_2_unsupported[8] = {0x4d, 0x45, 0x43, 0x50, 2u, 5u, 0u, 0u};
    server_send(channel[1], version_2_unsupported, sizeof(version_2_unsupported), NULL, 0u);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 1000u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_UNSUPPORTED);
    CHECK(error && strstr(error, "another version"));
    maelys_egress_client_error_free(error);
    /* The reserved field is ignored in v1. */
    static const unsigned char reserved[8] = {0x4d, 0x45, 0x43, 0x50, 1u, 1u, 0xbe, 0xef};
    server_send(channel[1], reserved, sizeof(reserved), NULL, 0u);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 1000u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_DENIED);
    maelys_egress_client_error_free(error);
    (void)close(channel[0]);
    (void)close(channel[1]);
}

static void test_deadline_shuts_the_channel(void) {
    int channel[2];
    channel_pair(channel);
    int received = -1;
    char *error = NULL;
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 50u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_UNANSWERED);
    CHECK(received == -1 && error && strstr(error, "shut down"));
    maelys_egress_client_error_free(error);
    /* The request did reach the server. The channel is now shut on the
     * client's side: no further request can leave on it, so a late answer
     * has nothing to pair with. Whether the server's end reads end of file
     * differs between kernels for a datagram pair; what the contract needs
     * is the refusal here. */
    server_expect_request(channel[1], "example.com", 443u);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 50u, &received,
                                       &error) == MAELYS_EGRESS_CLIENT_ERR_IO);
    CHECK(received == -1 && error != NULL);
    maelys_egress_client_error_free(error);
    unsigned char byte;
    CHECK(recv(channel[1], &byte, 1u, MSG_DONTWAIT) <= 0);
    /* The descriptor is still the caller's to close. */
    CHECK(fcntl(channel[0], F_GETFD) >= 0);
    (void)close(channel[0]);
    (void)close(channel[1]);
}

static void test_channel_closed_by_server(void) {
    int channel[2];
    channel_pair(channel);
    (void)close(channel[1]);
    int received = -1;
    char *error = NULL;
    maelys_egress_client_result_t result = maelys_egress_client_connect(
        channel[0], "example.com", 443u, 1000u, &received, &error);
    CHECK(result == MAELYS_EGRESS_CLIENT_ERR_IO && received == -1 && error != NULL);
    maelys_egress_client_error_free(error);
    (void)close(channel[0]);
}

static void test_arguments(void) {
    int received = 5;
    char *error = NULL;
    int channel[2];
    channel_pair(channel);
    CHECK(maelys_egress_client_connect(-1, "example.com", 443u, 0u, &received, &error) ==
          MAELYS_EGRESS_CLIENT_ERR_ARGUMENT);
    CHECK(received == -1 && error != NULL);
    maelys_egress_client_error_free(error);
    CHECK(maelys_egress_client_connect(channel[0], NULL, 443u, 0u, &received, &error) ==
          MAELYS_EGRESS_CLIENT_ERR_ARGUMENT);
    maelys_egress_client_error_free(error);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 0u, 0u, &received, &error) ==
          MAELYS_EGRESS_CLIENT_ERR_ARGUMENT);
    maelys_egress_client_error_free(error);
    CHECK(maelys_egress_client_connect(channel[0], "example com", 443u, 0u, &received, &error) ==
          MAELYS_EGRESS_CLIENT_ERR_ARGUMENT);
    maelys_egress_client_error_free(error);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 0u, NULL, &error) ==
          MAELYS_EGRESS_CLIENT_ERR_ARGUMENT);
    maelys_egress_client_error_free(error);
    CHECK(maelys_egress_client_connect(channel[0], "example.com", 443u, 0u, NULL, NULL) ==
          MAELYS_EGRESS_CLIENT_ERR_ARGUMENT);
    /* Nothing was sent for a refused call. */
    unsigned char byte;
    CHECK(recv(channel[1], &byte, 1u, MSG_DONTWAIT) < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK));
    (void)close(channel[0]);
    (void)close(channel[1]);
    CHECK(maelys_egress_client_abi_version() == MAELYS_EGRESS_CLIENT_ABI_VERSION);
    CHECK(strcmp(maelys_egress_client_result_string(MAELYS_EGRESS_CLIENT_ERR_UNANSWERED),
                 "unanswered") == 0);
    CHECK(strcmp(maelys_egress_client_result_string((maelys_egress_client_result_t)99),
                 "unknown") == 0);
    maelys_egress_client_error_free(NULL);
}

int main(void) {
    test_arguments();
    test_success_hands_over_one_stream();
    test_refusals_carry_no_stream();
    test_descriptor_cardinality();
    test_malformed_responses();
    test_deadline_shuts_the_channel();
    test_channel_closed_by_server();
    (void)printf("client: success, refusals, cardinality, malformed, deadline, closed channel ok\n");
    return 0;
}
