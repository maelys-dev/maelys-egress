/*
 * The server side of the mediated-connection channel: one thread per
 * channel, one authenticated connector, one request at a time. It is the
 * one file of the library that passes a descriptor to another process, and
 * for that it calls sendmsg and recvmsg itself: maelys-system has no
 * primitive for SCM_RIGHTS yet, and scripts/audit-boundaries.sh names this
 * file as the exception until it does. Everything else — the socket pair,
 * the wakeup, the loop, the thread, the closes — goes through maelys-system.
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "src/internal.h"
#include "maelys/egress_channel.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define CHANNEL_TOKEN_REQUESTS UINT64_C(1)
/* Room for every descriptor a kernel will ever deliver with one datagram:
 * macOS refuses more than 254 in its one SCM_RIGHTS header, Linux merges
 * headers and stops at 253. A request carries none by contract, but the
 * kernel does not know the contract: macOS installs whatever the peer
 * attached into this process even with no control buffer, with no flag to
 * say so, and only a buffer that can hold them all lets them be closed. */
#define CHANNEL_MAX_RIGHTS 256u
#define CHANNEL_TOKEN_WAKEUP UINT64_C(2)

/*
 * The loop belongs to the thread that steps it: registration, step and
 * destroy are owner-thread operations in maelys-system, and only wake and
 * stop cross threads. So the channel thread builds its own loop and reports
 * whether it could, through armed and armed_result under the lock, before
 * create returns to its caller.
 */
struct maelys_egress_channel {
    maelys_egress_connector_t *connector;
    uint64_t connect_timeout_ms;
    int server_fd;
    maelys_sys_wakeup_t *wakeup;
    maelys_sys_thread_t *thread;
    maelys_sys_mutex_t *lock;
    maelys_sys_condition_t *condition;
    int armed;
    maelys_sys_result_t armed_result;
};

static void report_armed(maelys_egress_channel_t *channel, maelys_sys_result_t result) {
    (void)maelys_sys_mutex_lock(channel->lock);
    channel->armed = 1;
    channel->armed_result = result;
    (void)maelys_sys_condition_broadcast(channel->condition);
    (void)maelys_sys_mutex_unlock(channel->lock);
}

/* The explicit table of the contract: what the connector reports, what the
 * wire says. Results that do not exist yet fall to INTERNAL. */
static maelys_egress_channel_status_t status_of(maelys_egress_result_t result) {
    switch (result) {
    case MAELYS_EGRESS_OK: return MAELYS_EGRESS_CHANNEL_OK;
    case MAELYS_EGRESS_ERR_DENIED: return MAELYS_EGRESS_CHANNEL_DENIED;
    case MAELYS_EGRESS_ERR_TIMEOUT: return MAELYS_EGRESS_CHANNEL_TIMEOUT;
    /* A server that is stopping cancels the open; one that has stopped, or
     * never ran, refuses it as a state. From the channel both read the
     * same: no session will come from this server. */
    case MAELYS_EGRESS_ERR_CANCELLED:
    case MAELYS_EGRESS_ERR_STATE: return MAELYS_EGRESS_CHANNEL_CANCELLED;
    case MAELYS_EGRESS_ERR_MEMORY: return MAELYS_EGRESS_CHANNEL_RESOURCE;
    /* The connector refused the request's shape: a host that is not
     * canonical passes the codec and fails here. */
    case MAELYS_EGRESS_ERR_ARGUMENT:
    case MAELYS_EGRESS_ERR_PROTOCOL: return MAELYS_EGRESS_CHANNEL_MALFORMED;
    default: return MAELYS_EGRESS_CHANNEL_INTERNAL;
    }
}

/* Sends one response, with the stream attached when there is one. Returns
 * whether the kernel took the datagram; the caller closes its copy of the
 * stream in every case, so a refused send leaves nothing open here. */
static int send_response(
    int server_fd, maelys_egress_channel_status_t status, int stream_fd) {
    unsigned char response[MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE];
    if (maelys_egress_channel_encode_response(status, response, sizeof(response)) !=
        MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE) {
        return 0;
    }
    struct iovec iov = {.iov_base = response, .iov_len = sizeof(response)};
    unsigned char control[CMSG_SPACE(sizeof(int))];
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1u;
    if (stream_fd >= 0) {
        memset(control, 0, sizeof(control));
        message.msg_control = control;
        message.msg_controllen = (socklen_t)CMSG_SPACE(sizeof(int));
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = (socklen_t)CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &stream_fd, sizeof(stream_fd));
    }
    ssize_t sent;
    do {
        sent = sendmsg(server_fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    return sent == (ssize_t)sizeof(response);
}

/* Closes every descriptor the received control data carries, reading no
 * further than the control bytes the kernel filled: a header may announce
 * more than was delivered when the kernel truncated. Returns how many. */
static size_t close_received_rights(const struct msghdr *message) {
    size_t closed = 0u;
    const unsigned char *end = (const unsigned char *)message->msg_control +
        message->msg_controllen;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(message); header;
         header = CMSG_NXTHDR((struct msghdr *)message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0)) {
            continue;
        }
        const unsigned char *data = CMSG_DATA(header);
        size_t declared = header->cmsg_len - CMSG_LEN(0);
        size_t available = data < end ? (size_t)(end - data) : 0u;
        size_t count = (declared < available ? declared : available) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int descriptor = -1;
            memcpy(&descriptor, data + i * sizeof(int), sizeof(descriptor));
            (void)maelys_sys_fd_close(&descriptor);
            ++closed;
        }
    }
    return closed;
}

/* Reads one datagram and answers it. Returns 0 when the channel is gone. */
static int serve_one(maelys_egress_channel_t *channel) {
    unsigned char request[MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE + 1u];
    struct iovec iov = {.iov_base = request, .iov_len = sizeof(request)};
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int) * CHANNEL_MAX_RIGHTS)];
    } control;
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    memset(&control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1u;
    message.msg_control = control.bytes;
    message.msg_controllen = (socklen_t)sizeof(control.bytes);
    ssize_t received;
    do {
        received = recvmsg(channel->server_fd, &message, 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0) return errno == EAGAIN || errno == EWOULDBLOCK;
    /* A request carries no descriptor. Whatever the peer attached is closed
     * here before anything else, and the request is refused: a confined
     * process must not be able to fill this process's descriptor table. */
    size_t attached = close_received_rights(&message);
    maelys_egress_channel_request_t decoded;
    maelys_egress_channel_status_t status;
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || attached != 0u ||
        message.msg_controllen != 0u) {
        status = MAELYS_EGRESS_CHANNEL_MALFORMED;
    } else {
        status = maelys_egress_channel_decode_request(
            request, (size_t)received, &decoded, NULL);
    }
    int stream_fd = -1;
    maelys_egress_session_t *session = NULL;
    if (status == MAELYS_EGRESS_CHANNEL_OK) {
        char *error = NULL;
        maelys_egress_result_t result = maelys_egress_connector_session_open(
            channel->connector, decoded.host, decoded.port,
            channel->connect_timeout_ms, &session, &error);
        maelys_egress_error_free(error);
        if (result == MAELYS_EGRESS_OK) {
            result = maelys_egress_session_take_fd(session, &stream_fd, NULL);
        }
        status = status_of(result);
    }
    /* Whether or not the kernel took the datagram, the copy here closes and
     * the session is released: the client end of the relay is then gone
     * when the send failed, and Egress treats the session as any client
     * that closed before writing. Delivery is confirmed only by the client's
     * own receipt of the message; the contract has no acknowledgement. */
    (void)send_response(channel->server_fd, status, stream_fd);
    (void)maelys_sys_fd_close(&stream_fd);
    maelys_egress_session_release(session);
    return 1;
}

static void *channel_main(void *opaque) {
    maelys_egress_channel_t *channel = opaque;
    maelys_sys_loop_t *loop = NULL;
    maelys_sys_watch_t requests_watch = 0u;
    maelys_sys_watch_t wakeup_watch = 0u;
    maelys_sys_result_t armed = maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop);
    if (armed == MAELYS_SYS_OK) {
        armed = maelys_sys_loop_watch_fd(loop, channel->server_fd, MAELYS_SYS_INTEREST_READ,
                                         CHANNEL_TOKEN_REQUESTS, &requests_watch);
    }
    if (armed == MAELYS_SYS_OK) {
        armed = maelys_sys_loop_watch_fd(loop, maelys_sys_wakeup_fd(channel->wakeup),
                                         MAELYS_SYS_INTEREST_READ, CHANNEL_TOKEN_WAKEUP,
                                         &wakeup_watch);
    }
    report_armed(channel, armed);
    int serving = armed == MAELYS_SYS_OK;
    maelys_sys_event_t events[4];
    while (serving) {
        size_t count = 0u;
        maelys_sys_step_result_t step;
        if (maelys_sys_loop_step(loop, MAELYS_SYS_DEADLINE_INFINITE, events,
                                 sizeof(events) / sizeof(events[0]), &count, &step) !=
            MAELYS_SYS_OK) {
            break;
        }
        for (size_t i = 0; serving && i < count; ++i) {
            if (events[i].token == CHANNEL_TOKEN_WAKEUP) {
                serving = 0;
            } else if (events[i].token == CHANNEL_TOKEN_REQUESTS) {
                if ((events[i].flags & MAELYS_SYS_EVENT_READ) && !serve_one(channel)) serving = 0;
                if (events[i].flags & (MAELYS_SYS_EVENT_HUP | MAELYS_SYS_EVENT_ERROR)) serving = 0;
            }
        }
    }
    if (loop) {
        if (wakeup_watch) (void)maelys_sys_loop_unwatch(loop, wakeup_watch);
        if (requests_watch) (void)maelys_sys_loop_unwatch(loop, requests_watch);
        (void)maelys_sys_loop_destroy(&loop);
    }
    return NULL;
}

maelys_egress_result_t maelys_egress_channel_create(
    maelys_egress_connector_t *connector,
    uint64_t connect_timeout_ms,
    maelys_egress_channel_t **out_channel,
    int *out_client_fd,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_channel) *out_channel = NULL;
    if (out_client_fd) *out_client_fd = -1;
    if (!connector || !connect_timeout_ms || !out_channel || !out_client_fd) {
        egress_set_error(out_error,
            "a channel needs a connector, a finite non-zero connect timeout and outputs");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    maelys_egress_channel_t *channel = calloc(1, sizeof(*channel));
    if (!channel) return MAELYS_EGRESS_ERR_MEMORY;
    channel->server_fd = -1;
    channel->connect_timeout_ms = connect_timeout_ms;
    int pair[2] = {-1, -1};
    maelys_egress_result_t result = MAELYS_EGRESS_ERR_IO;
    if (maelys_sys_socketpair_cloexec(SOCK_DGRAM, pair) != MAELYS_SYS_OK) {
        egress_set_error(out_error, "cannot create the channel pair: %s", strerror(errno));
        goto fail;
    }
    channel->server_fd = pair[0];
    if (maelys_sys_wakeup_create(&channel->wakeup) != MAELYS_SYS_OK ||
        maelys_sys_mutex_create(&channel->lock) != MAELYS_SYS_OK ||
        maelys_sys_condition_create(&channel->condition) != MAELYS_SYS_OK) {
        egress_set_error(out_error, "cannot arm the channel");
        goto fail;
    }
    channel->connector = connector;
    maelys_egress_connector_retain(connector);
    if (maelys_sys_thread_create("egress-channel", channel_main, channel, &channel->thread) !=
        MAELYS_SYS_OK) {
        egress_set_error(out_error, "cannot start the channel thread");
        goto fail;
    }
    (void)maelys_sys_mutex_lock(channel->lock);
    while (!channel->armed) (void)maelys_sys_condition_wait(channel->condition, channel->lock);
    maelys_sys_result_t armed_result = channel->armed_result;
    (void)maelys_sys_mutex_unlock(channel->lock);
    if (armed_result != MAELYS_SYS_OK) {
        egress_set_error(out_error, "cannot arm the channel reactor");
        goto fail;
    }
    *out_channel = channel;
    *out_client_fd = pair[1];
    return MAELYS_EGRESS_OK;

fail:
    (void)maelys_sys_fd_close(&pair[1]);
    maelys_egress_channel_destroy(channel);
    return result;
}

void maelys_egress_channel_destroy(maelys_egress_channel_t *channel) {
    if (!channel) return;
    if (channel->thread) {
        (void)maelys_sys_wakeup_signal(channel->wakeup);
        (void)maelys_sys_thread_join(&channel->thread, NULL);
    }
    maelys_sys_condition_destroy(channel->condition);
    maelys_sys_mutex_destroy(channel->lock);
    maelys_sys_wakeup_destroy(channel->wakeup);
    (void)maelys_sys_fd_close(&channel->server_fd);
    maelys_egress_connector_release(channel->connector);
    free(channel);
}
