/* SPDX-License-Identifier: MPL-2.0 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "maelys/egress_client.h"
#include "maelys/sys/fdpass.h"
#include "common/bootstrap.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct maelys_egress_client_channel {
    int lease;
    int channel;
    uint64_t connect_timeout_ms;
};

static uint64_t now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Re-evaluate one deadline even when readiness or EINTR keeps arriving. */
static int await_io(int fd, short events, uint64_t deadline) {
    for (;;) {
        uint64_t now = now_ms();
        if (now == UINT64_MAX) return -1;
        if (now >= deadline) return 0;
        uint64_t left = deadline - now;
        struct pollfd item = {.fd = fd, .events = events};
        int result = poll(&item, 1u, left > INT_MAX ? INT_MAX : (int)left);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
        if (result > 0) return 1;
    }
}

static maelys_egress_client_result_t status_result(unsigned status) {
    switch (status) {
    case EGRESS_BOOTSTRAP_OK: return MAELYS_EGRESS_CLIENT_OK;
    case EGRESS_BOOTSTRAP_DENIED: return MAELYS_EGRESS_CLIENT_ERR_DENIED;
    case EGRESS_BOOTSTRAP_BUSY: return MAELYS_EGRESS_CLIENT_ERR_BUSY;
    case EGRESS_BOOTSTRAP_CANCELLED: return MAELYS_EGRESS_CLIENT_ERR_CANCELLED;
    case EGRESS_BOOTSTRAP_MALFORMED: return MAELYS_EGRESS_CLIENT_ERR_PROTOCOL;
    case EGRESS_BOOTSTRAP_UNSUPPORTED: return MAELYS_EGRESS_CLIENT_ERR_UNSUPPORTED;
    case EGRESS_BOOTSTRAP_RESOURCE: return MAELYS_EGRESS_CLIENT_ERR_RESOURCE;
    default: return MAELYS_EGRESS_CLIENT_ERR_INTERNAL;
    }
}

static int is_channel(int fd) {
    int type = 0;
    socklen_t length = sizeof(type);
    struct sockaddr_storage peer;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 ||
        type != SOCK_DGRAM) return 0;
    length = sizeof(peer);
    return getpeername(fd, (struct sockaddr *)&peer, &length) == 0 &&
        peer.ss_family == AF_UNIX && (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
}

void maelys_egress_client_channel_close(maelys_egress_client_channel_t *channel) {
    if (!channel) return;
    if (channel->channel >= 0) (void)close(channel->channel);
    if (channel->lease >= 0) (void)close(channel->lease);
    free(channel);
}

int maelys_egress_client_channel_fd(const maelys_egress_client_channel_t *channel) {
    return channel ? channel->channel : -1;
}

uint64_t maelys_egress_client_channel_connect_timeout_ms(
    const maelys_egress_client_channel_t *channel) {
    return channel ? channel->connect_timeout_ms : 0u;
}

maelys_egress_client_result_t maelys_egress_client_channel_open(
    const char *path, uint64_t timeout,
    maelys_egress_client_channel_t **out_channel, char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_channel) *out_channel = NULL;
    maelys_egress_client_result_t result = MAELYS_EGRESS_CLIENT_ERR_ARGUMENT;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    maelys_egress_client_channel_t *channel = NULL;
    if (!out_channel || !timeout || timeout == UINT64_MAX ||
        !egress_bootstrap_path_valid(path) || strlen(path) >= sizeof(address.sun_path))
        goto fail;
    uint64_t started = now_ms();
    result = MAELYS_EGRESS_CLIENT_ERR_IO;
    if (started == UINT64_MAX) goto fail;
    uint64_t deadline = timeout >= UINT64_MAX - started ? UINT64_MAX - 1u : started + timeout;
    channel = calloc(1u, sizeof(*channel));
    if (!channel) { result = MAELYS_EGRESS_CLIENT_ERR_RESOURCE; goto fail; }
    channel->channel = -1;
#ifdef __linux__
    channel->lease = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
    channel->lease = socket(AF_UNIX, SOCK_STREAM, 0);
#endif
    if (channel->lease < 0 || fcntl(channel->lease, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(channel->lease, F_SETFL, O_NONBLOCK) != 0) goto fail;
    memcpy(address.sun_path, path, strlen(path) + 1u);
    int connected = connect(channel->lease, (struct sockaddr *)&address,
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1u));
    if (connected != 0) {
        /* EAGAIN on Linux Unix sockets means no connection was started,
         * not EINPROGRESS. Fail locally; never mistake writable for connected. */
        if (errno != EINPROGRESS && errno != EINTR) goto fail;
        int ready = await_io(channel->lease, POLLOUT, deadline);
        if (!ready) { result = MAELYS_EGRESS_CLIENT_ERR_UNANSWERED; goto fail; }
        if (ready < 0) goto fail;
        int error = 0;
        socklen_t length = sizeof(error);
        struct sockaddr_un peer;
        if (getsockopt(channel->lease, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error)
            goto fail;
        length = sizeof(peer);
        if (getpeername(channel->lease, (struct sockaddr *)&peer, &length) != 0) goto fail;
    }
    unsigned char request[EGRESS_BOOTSTRAP_REQUEST_SIZE];
    egress_bootstrap_request(request);
    size_t progress = 0u;
    while (progress < sizeof(request)) {
        int ready = await_io(channel->lease, POLLOUT, deadline);
        if (!ready) { result = MAELYS_EGRESS_CLIENT_ERR_UNANSWERED; goto fail; }
        if (ready < 0) goto fail;
        size_t sent = 0u;
        maelys_sys_result_t io = maelys_sys_fd_stream_send(channel->lease,
            request + progress, sizeof(request) - progress, -1, &sent);
        if (io == MAELYS_SYS_ERR_WOULD_BLOCK || (io == MAELYS_SYS_ERR_OS && errno == EINTR))
            continue;
        /* A saturated broker may queue BUSY and close before this write.
         * Linux can then report EPIPE although the refusal is readable. Do
         * not discard it; accept only a refusal if the request was partial. */
        if (io == MAELYS_SYS_ERR_CLOSED || io == MAELYS_SYS_ERR_RESET) break;
        if (io != MAELYS_SYS_OK || !sent) goto fail;
        progress += sent;
    }
    int request_complete = progress == sizeof(request);
    unsigned char response[EGRESS_BOOTSTRAP_RESPONSE_SIZE];
    progress = 0u;
    while (progress < sizeof(response)) {
        int ready = await_io(channel->lease, POLLIN, deadline);
        if (!ready) { result = MAELYS_EGRESS_CLIENT_ERR_UNANSWERED; goto fail; }
        if (ready < 0) goto fail;
        size_t received = 0u, count = 0u;
        unsigned flags = 0u;
        int passed = -1;
        /* Isolate byte zero: rights later in the frame are not its capability. */
        maelys_sys_result_t io = maelys_sys_fd_stream_receive(channel->lease,
            response + progress, progress ? sizeof(response) - progress : 1u,
            &received, &passed, 1u, &count, &flags);
        if (io == MAELYS_SYS_ERR_WOULD_BLOCK || (io == MAELYS_SYS_ERR_OS && errno == EINTR))
            continue;
        if (io != MAELYS_SYS_OK) goto fail;
        int malformed = flags || !received || (count && (progress || channel->channel >= 0));
        if (count) {
            if (malformed) (void)close(passed);
            else channel->channel = passed;
        }
        if (malformed) { result = MAELYS_EGRESS_CLIENT_ERR_PROTOCOL; goto fail; }
        progress += received;
    }
    unsigned status = EGRESS_BOOTSTRAP_INTERNAL;
    result = MAELYS_EGRESS_CLIENT_ERR_PROTOCOL;
    if (!egress_bootstrap_decode_response(response, sizeof(response), &status,
            &channel->connect_timeout_ms) ||
        (status == EGRESS_BOOTSTRAP_OK && !request_complete) ||
        ((status == EGRESS_BOOTSTRAP_OK) != (channel->channel >= 0))) goto fail;
    result = status_result(status);
    if (result != MAELYS_EGRESS_CLIENT_OK) goto fail;
    if (!is_channel(channel->channel)) { result = MAELYS_EGRESS_CLIENT_ERR_PROTOCOL; goto fail; }
    /* A fixed frame must not silently accept a queued tail (including rights).
     * Future peer closure is observed by the next channel operation. */
    unsigned char extra; size_t received = 0u, count = 0u; unsigned flags = 0u;
    maelys_sys_result_t tail = maelys_sys_fd_stream_receive(channel->lease,
        &extra, 1u, &received, NULL, 0u, &count, &flags);
    if (tail != MAELYS_SYS_ERR_WOULD_BLOCK && !(tail == MAELYS_SYS_ERR_OS && errno == EINTR)) {
        result = tail == MAELYS_SYS_OK ? MAELYS_EGRESS_CLIENT_ERR_PROTOCOL : MAELYS_EGRESS_CLIENT_ERR_IO;
        goto fail;
    }
    *out_channel = channel;
    return MAELYS_EGRESS_CLIENT_OK;
fail:
    maelys_egress_client_channel_close(channel);
    if (out_error) *out_error = strdup(maelys_egress_client_result_string(result));
    return result;
}
