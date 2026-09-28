/* SPDX-License-Identifier: MPL-2.0 */
/*
 * The channel client. This file and the codec are the whole of
 * libmaelys_egress_client; both use the C library alone, and
 * scripts/audit-boundaries.sh keeps it so. The descriptor handling here is
 * deliberately the caller's: the channel stays the caller's to close, the
 * stream becomes the caller's on success, and nothing else is kept.
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "maelys/egress_client.h"
#include "maelys/egress_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Room for every descriptor a kernel will ever deliver with one datagram:
 * macOS refuses more than 254 in its one SCM_RIGHTS header, Linux merges
 * headers and stops at 253. The contract allows one; a server in breach may
 * send more, and macOS installs them all even when they do not fit the
 * buffer, so only a buffer that holds them all lets the client close them. */
#define CLIENT_MAX_RIGHTS 256u

static void set_error(char **out_error, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void set_error(char **out_error, const char *format, ...) {
    if (!out_error) return;
    *out_error = NULL;
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length >= 0) {
        char *message = malloc((size_t)length + 1u);
        if (message) (void)vsnprintf(message, (size_t)length + 1u, format, args);
        *out_error = message;
    }
    va_end(args);
}

static void close_quietly(int *fd) {
    if (*fd >= 0) {
        int saved = errno;
        (void)close(*fd);
        errno = saved;
        *fd = -1;
    }
}

static maelys_egress_client_result_t from_status(uint8_t status) {
    switch (status) {
    case MAELYS_EGRESS_CHANNEL_OK: return MAELYS_EGRESS_CLIENT_OK;
    case MAELYS_EGRESS_CHANNEL_DENIED: return MAELYS_EGRESS_CLIENT_ERR_DENIED;
    case MAELYS_EGRESS_CHANNEL_TIMEOUT: return MAELYS_EGRESS_CLIENT_ERR_TIMEOUT;
    case MAELYS_EGRESS_CHANNEL_CANCELLED: return MAELYS_EGRESS_CLIENT_ERR_CANCELLED;
    /* The server says our request broke the contract: from where the caller
     * stands, that is the protocol failing, not the policy. */
    case MAELYS_EGRESS_CHANNEL_MALFORMED: return MAELYS_EGRESS_CLIENT_ERR_PROTOCOL;
    case MAELYS_EGRESS_CHANNEL_UNSUPPORTED: return MAELYS_EGRESS_CLIENT_ERR_UNSUPPORTED;
    case MAELYS_EGRESS_CHANNEL_RESOURCE: return MAELYS_EGRESS_CLIENT_ERR_RESOURCE;
    default: return MAELYS_EGRESS_CLIENT_ERR_INTERNAL;
    }
}

static int send_request(int channel_fd, const unsigned char *request, size_t length) {
    ssize_t sent;
    do {
        sent = send(channel_fd, request, length, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    return sent == (ssize_t)length;
}

/* Waits for the response within the deadline. Returns 1 when readable, 0 on
 * the deadline, -1 on a poll failure. */
static int await_response(int channel_fd, uint64_t read_timeout_ms) {
    for (;;) {
        struct pollfd descriptor = {.fd = channel_fd, .events = POLLIN};
        int timeout = -1;
        if (read_timeout_ms) {
            timeout = read_timeout_ms > 0x7fffffffu ? 0x7fffffff : (int)read_timeout_ms;
        }
        int ready = poll(&descriptor, 1u, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        return ready > 0;
    }
}

maelys_egress_client_result_t maelys_egress_client_connect(
    int channel_fd,
    const char *host,
    uint16_t port,
    uint64_t read_timeout_ms,
    int *out_stream_fd,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_stream_fd) *out_stream_fd = -1;
    unsigned char request[MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE];
    size_t request_length = maelys_egress_channel_encode_request(
        host, port, request, sizeof(request));
    if (channel_fd < 0 || !out_stream_fd || request_length == 0u) {
        set_error(out_error, "channel connect needs a channel, a host of 1..253 printable "
                  "ASCII bytes, a port of 1..65535 and an output");
        return MAELYS_EGRESS_CLIENT_ERR_ARGUMENT;
    }
    if (!send_request(channel_fd, request, request_length)) {
        set_error(out_error, "cannot send the channel request: %s", strerror(errno));
        return MAELYS_EGRESS_CLIENT_ERR_IO;
    }
    int ready = await_response(channel_fd, read_timeout_ms);
    if (ready < 0) {
        set_error(out_error, "cannot wait for the channel response: %s", strerror(errno));
        return MAELYS_EGRESS_CLIENT_ERR_IO;
    }
    if (ready == 0) {
        /* A late answer would pair with the next request; end the channel
         * instead. The caller keeps the descriptor number and closes it. */
        (void)shutdown(channel_fd, SHUT_RDWR);
        set_error(out_error, "no channel response within %llu ms; the channel is shut down",
                  (unsigned long long)read_timeout_ms);
        return MAELYS_EGRESS_CLIENT_ERR_UNANSWERED;
    }

    unsigned char response[MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE + 1u];
    struct iovec iov = {.iov_base = response, .iov_len = sizeof(response)};
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int) * CLIENT_MAX_RIGHTS)];
    } control;
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    memset(&control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1u;
    message.msg_control = control.bytes;
    message.msg_controllen = (socklen_t)sizeof(control.bytes);
    int flags = 0;
#ifdef MSG_CMSG_CLOEXEC
    flags |= MSG_CMSG_CLOEXEC;
#endif
    ssize_t received;
    do {
        received = recvmsg(channel_fd, &message, flags);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        set_error(out_error, "cannot receive the channel response: %s", strerror(errno));
        return MAELYS_EGRESS_CLIENT_ERR_IO;
    }
    if (received == 0) {
        set_error(out_error, "the channel closed before answering");
        return MAELYS_EGRESS_CLIENT_ERR_IO;
    }

    /* Collect every descriptor first, whatever the data says: one that is
     * not kept must be closed, and a malformed message may still carry some. */
    int stream = -1;
    size_t rights = 0u;
    int malformed_rights = 0;
    /* Read no further than the control bytes the kernel filled: after a
     * truncation a header may announce more than was delivered. */
    const unsigned char *control_end = control.bytes + message.msg_controllen;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0) ||
            (header->cmsg_len - CMSG_LEN(0)) % sizeof(int) != 0u) {
            malformed_rights = 1;
            continue;
        }
        const unsigned char *data = CMSG_DATA(header);
        size_t declared = header->cmsg_len - CMSG_LEN(0);
        size_t available = data < control_end ? (size_t)(control_end - data) : 0u;
        size_t count = (declared < available ? declared : available) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int descriptor = -1;
            memcpy(&descriptor, data + i * sizeof(int), sizeof(descriptor));
            if (rights++ == 0u) stream = descriptor;
            else close_quietly(&descriptor);
        }
    }
    if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
        close_quietly(&stream);
        set_error(out_error, "the channel response was truncated");
        return MAELYS_EGRESS_CLIENT_ERR_PROTOCOL;
    }
    maelys_egress_channel_response_t decoded;
    const char *reason = NULL;
    if (!maelys_egress_channel_decode_response(response, (size_t)received, &decoded, &reason)) {
        close_quietly(&stream);
        set_error(out_error, "malformed channel response: %s", reason ? reason : "unknown");
        return MAELYS_EGRESS_CLIENT_ERR_PROTOCOL;
    }
    /* UNSUPPORTED is how a server of another version says so; it is read
     * before the version check so that the caller learns which. */
    if (decoded.status != MAELYS_EGRESS_CHANNEL_UNSUPPORTED &&
        decoded.version != MAELYS_EGRESS_CHANNEL_PROTOCOL_VERSION) {
        close_quietly(&stream);
        set_error(out_error, "channel response of version %u, this client speaks 1",
                  (unsigned int)decoded.version);
        return MAELYS_EGRESS_CLIENT_ERR_PROTOCOL;
    }
    maelys_egress_client_result_t result = from_status(decoded.status);
    if (result != MAELYS_EGRESS_CLIENT_OK) {
        int carried = rights != 0u || malformed_rights;
        close_quietly(&stream);
        if (carried) {
            set_error(out_error, "channel response %s carried a descriptor",
                      maelys_egress_channel_status_string(
                          (maelys_egress_channel_status_t)decoded.status));
            return MAELYS_EGRESS_CLIENT_ERR_PROTOCOL;
        }
        set_error(out_error, "channel refused the destination: %s%s",
                  maelys_egress_channel_status_string(
                      (maelys_egress_channel_status_t)decoded.status),
                  decoded.status == MAELYS_EGRESS_CHANNEL_UNSUPPORTED &&
                  decoded.version != MAELYS_EGRESS_CHANNEL_PROTOCOL_VERSION ?
                  " (the server speaks another version)" : "");
        return result;
    }
    if (malformed_rights || rights != 1u || stream < 0) {
        close_quietly(&stream);
        set_error(out_error, "channel response OK carried %zu descriptors, not one", rights);
        return MAELYS_EGRESS_CLIENT_ERR_PROTOCOL;
    }
    int descriptor_flags = fcntl(stream, F_GETFD);
    if (descriptor_flags < 0 || fcntl(stream, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        close_quietly(&stream);
        set_error(out_error, "cannot mark the relayed stream CLOEXEC: %s", strerror(errno));
        return MAELYS_EGRESS_CLIENT_ERR_IO;
    }
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    (void)setsockopt(stream, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    *out_stream_fd = stream;
    return MAELYS_EGRESS_CLIENT_OK;
}

const char *maelys_egress_client_result_string(maelys_egress_client_result_t result) {
    switch (result) {
    case MAELYS_EGRESS_CLIENT_OK: return "ok";
    case MAELYS_EGRESS_CLIENT_ERR_ARGUMENT: return "argument";
    case MAELYS_EGRESS_CLIENT_ERR_IO: return "io";
    case MAELYS_EGRESS_CLIENT_ERR_UNANSWERED: return "unanswered";
    case MAELYS_EGRESS_CLIENT_ERR_PROTOCOL: return "protocol";
    case MAELYS_EGRESS_CLIENT_ERR_DENIED: return "denied";
    case MAELYS_EGRESS_CLIENT_ERR_TIMEOUT: return "timeout";
    case MAELYS_EGRESS_CLIENT_ERR_CANCELLED: return "cancelled";
    case MAELYS_EGRESS_CLIENT_ERR_UNSUPPORTED: return "unsupported";
    case MAELYS_EGRESS_CLIENT_ERR_RESOURCE: return "resource";
    case MAELYS_EGRESS_CLIENT_ERR_INTERNAL: return "internal";
    }
    return "unknown";
}

void maelys_egress_client_error_free(char *error) { free(error); }

unsigned int maelys_egress_client_abi_version(void) {
    return MAELYS_EGRESS_CLIENT_ABI_VERSION;
}
