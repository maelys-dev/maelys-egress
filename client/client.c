/* SPDX-License-Identifier: MPL-2.0 */
/*
 * The channel client. This file, the codec and maelys-system's fdpass are
 * the whole of libmaelys_egress_client. fdpass.o comes from the pinned
 * libmaelys_sys.a as the very object the library holds, and names nothing
 * else of it; scripts/audit-boundaries.sh keeps this file to fdpass alone,
 * and the build refuses any undefined maelys_sys_ or pthread_ symbol in the
 * archive, so a confined process inherits neither the loop, nor the
 * threads, nor the files of maelys-system. The descriptor handling here is
 * deliberately the caller's: the channel stays the caller's to close, the
 * stream becomes the caller's on success, and nothing else is kept.
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "maelys/egress_client.h"
#include "maelys/egress_channel.h"
#include "maelys/sys/fdpass.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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

/* What a failed fdpass call means, in the client's words: the client may
 * name fdpass alone, not the rest of maelys-system's vocabulary. */
static const char *transfer_failure(maelys_sys_result_t result) {
    switch (result) {
    case MAELYS_SYS_ERR_CLOSED: return "the channel's other end is gone";
    case MAELYS_SYS_ERR_WOULD_BLOCK: return "the channel's queue is full";
    case MAELYS_SYS_ERR_ARGUMENT: return "the descriptor is not a connected datagram channel";
    case MAELYS_SYS_ERR_OS: return strerror(errno);
    default: return "the transfer failed";
    }
}

static int elapsed_ms(
    const struct timespec *started, const struct timespec *now, uint64_t *out_elapsed) {
    if (now->tv_sec < started->tv_sec ||
        (now->tv_sec == started->tv_sec && now->tv_nsec < started->tv_nsec)) {
        errno = EIO;
        return 0;
    }
    uint64_t seconds = (uint64_t)(now->tv_sec - started->tv_sec);
    long nanoseconds = now->tv_nsec - started->tv_nsec;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += 1000000000L;
    }
    if (seconds > UINT64_MAX / 1000u) {
        *out_elapsed = UINT64_MAX;
    } else {
        *out_elapsed = seconds * 1000u + (uint64_t)nanoseconds / 1000000u;
    }
    return 1;
}

/* Waits for the response within one monotonic deadline. Returns 1 when
 * readable, 0 on the deadline, -1 on a clock or poll failure. */
static int await_response(int channel_fd, uint64_t read_timeout_ms) {
    struct timespec started = {0};
    if (read_timeout_ms && clock_gettime(CLOCK_MONOTONIC, &started) != 0) return -1;
    for (;;) {
        struct pollfd descriptor = {.fd = channel_fd, .events = POLLIN};
        int timeout = -1;
        if (read_timeout_ms) {
            struct timespec now;
            uint64_t elapsed = 0u;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
                !elapsed_ms(&started, &now, &elapsed)) {
                return -1;
            }
            if (elapsed >= read_timeout_ms) return 0;
            uint64_t remaining = read_timeout_ms - elapsed;
            timeout = remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
        }
        int ready = poll(&descriptor, 1u, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        if (ready > 0) return 1;
        /* A finite wait is split at INT_MAX and is also rechecked against
         * the monotonic origin in case poll rounded or returned early. */
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
    maelys_sys_result_t sent = maelys_sys_fd_send(channel_fd, request, request_length, -1);
    if (sent != MAELYS_SYS_OK) {
        set_error(out_error, "cannot send the channel request: %s", transfer_failure(sent));
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

    /* One descriptor is room enough: fdpass reads with room for all a
     * kernel can deliver, closes those beyond this one and says SURPLUS,
     * and sets close-on-exec on the one it returns. */
    unsigned char response[MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE + 1u];
    size_t received = 0u;
    int descriptors[1] = {-1};
    size_t rights = 0u;
    unsigned flags = 0u;
    maelys_sys_result_t got = maelys_sys_fd_receive(
        channel_fd, response, sizeof(response), &received,
        descriptors, 1u, &rights, &flags);
    if (got != MAELYS_SYS_OK) {
        set_error(out_error, "cannot receive the channel response: %s", transfer_failure(got));
        return MAELYS_EGRESS_CLIENT_ERR_IO;
    }
    int stream = rights ? descriptors[0] : -1;
    int surplus = (flags & MAELYS_SYS_FDPASS_SURPLUS) != 0u;
    if (flags & (MAELYS_SYS_FDPASS_TRUNCATED | MAELYS_SYS_FDPASS_CONTROL_TRUNCATED)) {
        close_quietly(&stream);
        set_error(out_error, "the channel response was truncated");
        return MAELYS_EGRESS_CLIENT_ERR_PROTOCOL;
    }
    maelys_egress_channel_response_t decoded;
    const char *reason = NULL;
    if (!maelys_egress_channel_decode_response(response, received, &decoded, &reason)) {
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
        int carried = rights != 0u || surplus;
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
    if (surplus || rights != 1u || stream < 0) {
        close_quietly(&stream);
        if (surplus) {
            set_error(out_error, "channel response OK carried more than one descriptor");
        } else {
            set_error(out_error, "channel response OK carried no descriptor");
        }
        return MAELYS_EGRESS_CLIENT_ERR_PROTOCOL;
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
