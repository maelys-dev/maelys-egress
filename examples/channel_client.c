/*
 * The confined side of the mediated-connection channel. This program links
 * libmaelys_egress_client alone — no Egress core, no maelys-system — which
 * is the point: nothing of the proxy reaches the process being confined.
 * Its supervisor hands it the channel on a descriptor and names the number
 * in MAELYS_EGRESS_CHANNEL_FD, as `maelys-egress channel exec` does; it asks
 * for one destination and talks to the stream it gets back.
 *
 *   example-channel_client HOST PORT
 *
 * MAELYS_EGRESS_CHANNEL_CONNECT_TIMEOUT_MS, when the supervisor sets it, is
 * how long Egress may take to reach a destination: the client waits a little
 * longer than that, so that a refusal arrives as an answer and not as a
 * timeout of its own.
 */
#include <maelys/egress_client.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* A decimal number in [minimum, maximum] from the environment, or fallback
 * when the variable is absent. A value that is present and wrong is an
 * error of the supervisor, never something to guess around: -1. */
static long long environment_number(const char *name, long long minimum,
    long long maximum, long long fallback) {
    const char *text = getenv(name);
    if (!text) return fallback;
    char *end = NULL;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (!*text || *end || errno || value < minimum || value > maximum) return -1;
    return value;
}

static int send_all(int fd, const void *data, size_t length) {
    const unsigned char *cursor = data;
    while (length) {
        ssize_t amount = send(fd, cursor, length, 0);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return 0;
        cursor += (size_t)amount;
        length -= (size_t)amount;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s HOST PORT\n", argv[0]);
        return 2;
    }
    long long channel_fd = environment_number("MAELYS_EGRESS_CHANNEL_FD", 3, 255, -1);
    long long connect_ms = environment_number("MAELYS_EGRESS_CHANNEL_CONNECT_TIMEOUT_MS",
        1, 600000, 5000);
    if (channel_fd < 0 || connect_ms < 0) {
        fprintf(stderr, "MAELYS_EGRESS_CHANNEL_FD must name the inherited channel "
                        "descriptor; start this program through its supervisor\n");
        return 2;
    }
    /* The channel is this process's alone: what it starts must not inherit it. */
    (void)fcntl((int)channel_fd, F_SETFD, FD_CLOEXEC);
    unsigned long port = strtoul(argv[2], NULL, 10);
    int stream = -1;
    char *error = NULL;
    maelys_egress_client_result_t result = maelys_egress_client_connect(
        (int)channel_fd, argv[1], (uint16_t)port, (uint64_t)connect_ms + 1000u, &stream, &error);
    if (result != MAELYS_EGRESS_CLIENT_OK) {
        fprintf(stderr, "channel connect: %s: %s\n",
                maelys_egress_client_result_string(result), error ? error : "no detail");
        maelys_egress_client_error_free(error);
        return 1;
    }
    static const char hello[] = "hello through the channel\n";
    char echo[64] = {0};
    ssize_t received = send_all(stream, hello, sizeof(hello) - 1u) ?
        recv(stream, echo, sizeof(echo) - 1u, 0) : -1;
    (void)close(stream);
    if (received <= 0) {
        fprintf(stderr, "the stream carried nothing back\n");
        return 1;
    }
    printf("client: the stream answered %zd bytes: %s", received, echo);
    return 0;
}
