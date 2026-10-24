/*
 * The server side of the mediated-connection channel: one thread per
 * channel, one authenticated connector, one request at a time. The
 * descriptor crosses the process boundary through maelys-system's fdpass,
 * like everything else a socket does here — the pair, the wakeup, the loop,
 * the thread, the closes.
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "src/internal.h"
#include "maelys/egress_channel.h"
#include "maelys/sys/fdpass.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#define CHANNEL_TOKEN_REQUESTS UINT64_C(1)
#define CHANNEL_TOKEN_WAKEUP UINT64_C(2)

/*
 * On macOS a descriptor whose every reference is in flight in SCM_RIGHTS can
 * be flushed by the kernel's unp_gc while the client receives it, whenever
 * any process closes a Unix socket with rights in flight: the client gets a
 * stream that reads end of file while its peer is still connected, and the
 * relay's bytes are dropped. So the channel keeps its copy of the stream it
 * sent until the client shows it has it: its next request (a client asks
 * one at a time), the stream seen in use from this copy, which is the same
 * socket, or the end of the channel; CHANNEL_HOLD_MS bounds the wait. While
 * the copy is kept, a close by the client sends no FIN; a shutdown does.
 * Linux collects only Unix sockets in flight, without this race, and keeps
 * nothing.
 */
#define CHANNEL_HOLD_MS UINT64_C(1000)
#define CHANNEL_HOLD_PAUSE_MAX_MS UINT64_C(8)

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
    /* The stream of the last OK response, kept until the client has it
     * (macOS only; -1 elsewhere), with its bound and its next check. */
    int held_fd;
    uint64_t held_until;
    uint64_t held_check;
    uint64_t held_pause;
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
 * stream when it did not, so a refused send leaves nothing open here. */
static int send_response(
    int server_fd, maelys_egress_channel_status_t status, int stream_fd) {
    unsigned char response[MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE];
    if (maelys_egress_channel_encode_response(status, response, sizeof(response)) !=
        MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE) {
        return 0;
    }
    return maelys_sys_fd_send(server_fd, response, sizeof(response), stream_fd) ==
        MAELYS_SYS_OK;
}

static void release_held(maelys_egress_channel_t *channel) {
    (void)maelys_sys_fd_close(&channel->held_fd);
}

#if defined(__APPLE__)
/* Whether the client shows it has the stream: bytes it sent, or bytes it
 * read of those the relay sent. An unreadable state releases the copy. */
static int held_in_use(int fd) {
    struct tcp_connection_info info;
    socklen_t length = sizeof(info);
    int unread = 0;
    socklen_t unread_length = sizeof(unread);
    if (getsockopt(fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &length) != 0 ||
        getsockopt(fd, SOL_SOCKET, SO_NREAD, &unread, &unread_length) != 0 || unread < 0) {
        return 1;
    }
    return info.tcpi_txbytes > 0u || info.tcpi_rxbytes > (uint64_t)unread;
}
#endif

/* Takes the stream of a delivered response: kept on macOS, closed elsewhere
 * or when the clock fails. */
static void hold_stream(maelys_egress_channel_t *channel, int *stream_fd) {
#if defined(__APPLE__)
    uint64_t now = 0u;
    if (*stream_fd >= 0 && maelys_sys_monotonic_ms(&now) == MAELYS_SYS_OK) {
        channel->held_fd = *stream_fd;
        *stream_fd = -1;
        channel->held_until = now + CHANNEL_HOLD_MS;
        channel->held_pause = 1u;
        channel->held_check = now + channel->held_pause;
    }
#else
    (void)channel;
#endif
    (void)maelys_sys_fd_close(stream_fd);
}

/* Releases the kept stream once in use or at its bound, and otherwise sets
 * the next check, the pause doubling up to CHANNEL_HOLD_PAUSE_MAX_MS. */
static void check_held(maelys_egress_channel_t *channel) {
#if defined(__APPLE__)
    if (channel->held_fd < 0) return;
    uint64_t now = 0u;
    if (maelys_sys_monotonic_ms(&now) != MAELYS_SYS_OK || now >= channel->held_until ||
        held_in_use(channel->held_fd)) {
        release_held(channel);
        return;
    }
    if (now >= channel->held_check) {
        if (channel->held_pause < CHANNEL_HOLD_PAUSE_MAX_MS) channel->held_pause *= 2u;
        channel->held_check = now + channel->held_pause;
        if (channel->held_check > channel->held_until) channel->held_check = channel->held_until;
    }
#else
    (void)channel;
#endif
}

/* Reads one datagram and answers it. Returns 0 when the channel is gone. */
static int serve_one(maelys_egress_channel_t *channel) {
    unsigned char request[MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE + 1u];
    size_t received = 0u;
    size_t attached = 0u;
    unsigned flags = 0u;
    /* A request carries no descriptor, so none is asked for: fdpass reads
     * with room for all a kernel can deliver and closes every one of them,
     * and SURPLUS says some came. On macOS the kernel installs them in this
     * process whatever the receiver asks; without that, a confined process
     * could fill this process's descriptor table. */
    maelys_sys_result_t got = maelys_sys_fd_receive(
        channel->server_fd, request, sizeof(request), &received,
        NULL, 0u, &attached, &flags);
    if (got == MAELYS_SYS_ERR_WOULD_BLOCK) return 1;
    if (got != MAELYS_SYS_OK) return 0;
    /* A client asks once it has its previous answer: that stream is its. */
    release_held(channel);
    maelys_egress_channel_request_t decoded;
    maelys_egress_channel_status_t status;
    if (flags & (MAELYS_SYS_FDPASS_TRUNCATED | MAELYS_SYS_FDPASS_CONTROL_TRUNCATED |
                 MAELYS_SYS_FDPASS_SURPLUS)) {
        status = MAELYS_EGRESS_CHANNEL_MALFORMED;
    } else {
        status = maelys_egress_channel_decode_request(request, received, &decoded, NULL);
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
    /* The session is released and the copy here leaves it: closed when the
     * kernel refused the datagram, so that the client end of the relay is
     * gone and Egress treats the session as any client that closed before
     * writing; kept on macOS until the client has it when the datagram was
     * queued (hold_stream). A response the nonblocking channel cannot queue
     * ends the channel: leaving it open would leave this request unanswered
     * and let a client that does not read block its supervisor. The
     * contract has no acknowledgement. */
    int delivered = send_response(channel->server_fd, status, stream_fd);
    if (delivered) hold_stream(channel, &stream_fd);
    (void)maelys_sys_fd_close(&stream_fd);
    maelys_egress_session_release(session);
    return delivered;
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
        check_held(channel);
        uint64_t deadline = channel->held_fd >= 0 ? channel->held_check :
            MAELYS_SYS_DEADLINE_INFINITE;
        if (maelys_sys_loop_step(loop, deadline, events,
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
    /* Closing here makes every way the worker stops observable by the
     * client, including response backpressure. destroy joins before it
     * touches the descriptors again, so its closes remain idempotent. */
    release_held(channel);
    (void)maelys_sys_fd_close(&channel->server_fd);
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
    channel->held_fd = -1;
    channel->connect_timeout_ms = connect_timeout_ms;
    int pair[2] = {-1, -1};
    maelys_egress_result_t result = MAELYS_EGRESS_ERR_IO;
    if (maelys_sys_socketpair_cloexec(SOCK_DGRAM, pair) != MAELYS_SYS_OK) {
        egress_set_error(out_error, "cannot create the channel pair: %s", strerror(errno));
        goto fail;
    }
    channel->server_fd = pair[0];
    if (maelys_sys_fd_set_nonblocking(channel->server_fd) != MAELYS_SYS_OK) {
        egress_set_error(out_error, "cannot make the channel server nonblocking: %s",
                         strerror(errno));
        goto fail;
    }
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

void egress_channel_stop(maelys_egress_channel_t *channel) {
    if (channel && channel->thread) (void)maelys_sys_wakeup_signal(channel->wakeup);
}

void maelys_egress_channel_destroy(maelys_egress_channel_t *channel) {
    if (!channel) return;
    if (channel->thread) {
        egress_channel_stop(channel);
        (void)maelys_sys_thread_join(&channel->thread, NULL);
    }
    maelys_sys_condition_destroy(channel->condition);
    maelys_sys_mutex_destroy(channel->lock);
    maelys_sys_wakeup_destroy(channel->wakeup);
    release_held(channel);
    (void)maelys_sys_fd_close(&channel->server_fd);
    maelys_egress_connector_release(channel->connector);
    free(channel);
}
