/*
 * The confined side of the mediated-connection channel. This program links
 * libmaelys_egress_client alone — no Egress core, no maelys-system — which
 * is the point: nothing of the proxy reaches the process being confined.
 * Its supervisor hands it the channel on a descriptor and names the number;
 * it asks for one destination and talks to the stream it gets back.
 *
 *   example-channel_client CHANNEL_FD HOST PORT
 */
#include <maelys/egress_client.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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
    if (argc != 4) {
        fprintf(stderr, "usage: %s CHANNEL_FD HOST PORT\n", argv[0]);
        return 2;
    }
    int channel_fd = atoi(argv[1]);
    unsigned long port = strtoul(argv[3], NULL, 10);
    int stream = -1;
    char *error = NULL;
    maelys_egress_client_result_t result = maelys_egress_client_connect(
        channel_fd, argv[2], (uint16_t)port, 10000u, &stream, &error);
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
