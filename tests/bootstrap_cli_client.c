/* SPDX-License-Identifier: MPL-2.0 */
/* Test peer linked to the standalone client archive, never the Egress core. */
#include "maelys/egress_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    maelys_egress_client_channel_t *channel = NULL;
    maelys_egress_client_result_t result = maelys_egress_client_channel_open(argv[1], 3000u, &channel, NULL);
    if (result != MAELYS_EGRESS_CLIENT_OK) return 1;
    printf("ready %llu\n", (unsigned long long)maelys_egress_client_channel_connect_timeout_ms(channel));
    fflush(stdout);
    char line[300], host[254]; unsigned port;
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "%253s %u", host, &port) != 2 || !port || port > 65535u) return 2;
        int fd = -1;
        result = maelys_egress_client_connect(maelys_egress_client_channel_fd(channel),
            host, (unsigned short)port, 4000u, &fd, NULL);
        if (result == MAELYS_EGRESS_CLIENT_OK) {
            struct timeval timeout = {.tv_sec = 2};
            if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) return 1;
            char response[4]; size_t received = 0u;
            if (send(fd, "ping", 4u, 0) != 4) return 1;
            while (received < sizeof(response)) {
                ssize_t amount = recv(fd, response + received, sizeof(response) - received, 0);
                if (amount <= 0) return 1;
                received += (size_t)amount;
            }
            if (memcmp(response, "ping", 4u)) return 1;
            close(fd);
        }
        puts(maelys_egress_client_result_string(result)); fflush(stdout);
    }
    maelys_egress_client_channel_close(channel);
    return 0;
}
