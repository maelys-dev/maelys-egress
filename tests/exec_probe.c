/* SPDX-License-Identifier: MPL-2.0 */
/*
 * The program `channel exec` starts in tests/test_channel_exec.py. It links
 * libmaelys_egress_client alone, finds its channel the way the command
 * announces it, and reports what it sees on stdout, one line per fact.
 *
 *   exec-probe exit CODE
 *   exec-probe report [ARGUMENT...]
 *   exec-probe connect HOST PORT
 *   exec-probe hold HOST PORT     connect, print ready, echo again on SIGTERM
 *   exec-probe sleep              wait for a signal with default dispositions
 *   exec-probe leave HOST PORT FILE   leave a process holding a stream, exit 7
 *   exec-probe linger STREAM_FD FILE  that process: note in FILE how the stream ended
 *   exec-probe cat                copy stdin to stdout
 */
#include <maelys/egress_client.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

extern char **environ;

static volatile sig_atomic_t terminated;
static void on_term(int number) { (void)number; terminated = 1; }

static int channel_fd(void) {
    const char *text = getenv("MAELYS_EGRESS_CHANNEL_FD");
    char *end = NULL;
    long value = text ? strtol(text, &end, 10) : -1;
    if (!text || !*text || *end || value < 3 || value > 255) {
        fprintf(stderr, "MAELYS_EGRESS_CHANNEL_FD is not a descriptor number\n");
        exit(90);
    }
    return (int)value;
}

static uint64_t connect_timeout(void) {
    const char *text = getenv("MAELYS_EGRESS_CHANNEL_CONNECT_TIMEOUT_MS");
    return text ? strtoull(text, NULL, 10) : 0u;
}

static int open_stream(const char *host, const char *port) {
    int stream = -1;
    maelys_egress_client_result_t result = maelys_egress_client_connect(channel_fd(),
        host, (uint16_t)strtoul(port, NULL, 10), connect_timeout() + 1000u, &stream, NULL);
    if (result != MAELYS_EGRESS_CLIENT_OK) {
        printf("refused %s\n", maelys_egress_client_result_string(result));
        fflush(stdout);
        return -1;
    }
    return stream;
}

static int echo(int stream, const char *text) {
    char answer[32] = {0};
    size_t length = strlen(text);
    if (send(stream, text, length, 0) != (ssize_t)length) return 0;
    size_t received = 0u;
    while (received < length) {
        ssize_t amount = recv(stream, answer + received, length - received, 0);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return 0;
        received += (size_t)amount;
    }
    return memcmp(answer, text, length) == 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "exit") && argc == 3) return atoi(argv[2]);
    if (!strcmp(mode, "report")) {
        int fd = channel_fd(), type = 0;
        socklen_t length = sizeof(type);
        printf("fd %d\n", fd);
        printf("timeout %llu\n", (unsigned long long)connect_timeout());
        printf("type %s\n", getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 &&
            type == SOCK_DGRAM ? "datagram" : "other");
        printf("cloexec %d\n", (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
        printf("open");
        for (int other = 3; other < 256; ++other)
            if (fcntl(other, F_GETFD) >= 0) printf(" %d", other);
        printf("\n");
        for (int i = 2; i < argc; ++i) printf("argument [%s]\n", argv[i]);
        return 0;
    }
    if (!strcmp(mode, "connect") && argc == 4) {
        int stream = open_stream(argv[2], argv[3]);
        if (stream < 0) return 3;
        puts(echo(stream, "ping") ? "echo ok" : "echo failed");
        return 0;
    }
    if (!strcmp(mode, "hold") && argc == 4) {
        struct sigaction action = {0};
        action.sa_handler = on_term;
        (void)sigaction(SIGTERM, &action, NULL);
        (void)sigaction(SIGINT, &action, NULL);
        int stream = open_stream(argv[2], argv[3]);
        if (stream < 0 || !echo(stream, "before")) return 3;
        puts("ready");
        fflush(stdout);
        while (!terminated) (void)pause();
        /* The launcher has been told to stop and has forwarded it: the
         * relayed connection must still carry bytes. */
        puts(echo(stream, "after") ? "echo after signal ok" : "echo after signal failed");
        return 0;
    }
    if (!strcmp(mode, "sleep")) {
        puts("ready");
        fflush(stdout);
        for (;;) (void)pause();
    }
    if (!strcmp(mode, "leave") && argc == 5) {
        int stream = open_stream(argv[2], argv[3]);
        if (stream < 0 || !echo(stream, "before")) return 3;
        fflush(stdout);
        /* What the program leaves behind: a process of its own holding the
         * stream, and the channel with it, but neither of the launcher's
         * output streams. A fresh program rather than a fork, so that it
         * runs the same under a sanitizer's runtime. */
        char stream_text[16];
        (void)snprintf(stream_text, sizeof(stream_text), "%d", stream);
        char linger[] = "linger";
        char *left[] = {argv[0], linger, stream_text, argv[4], NULL};
        posix_spawn_file_actions_t actions;
        pid_t child = 0;
        if (fcntl(stream, F_SETFD, 0) != 0 || posix_spawn_file_actions_init(&actions) != 0) return 4;
        int spawned = posix_spawn_file_actions_addclose(&actions, STDOUT_FILENO) == 0 &&
            posix_spawn_file_actions_addclose(&actions, STDERR_FILENO) == 0 &&
            posix_spawn(&child, argv[0], &actions, NULL, left, environ) == 0;
        (void)posix_spawn_file_actions_destroy(&actions);
        return spawned ? 7 : 4;
    }
    if (!strcmp(mode, "linger") && argc == 4) {
        /* It loses its connection when the launcher destroys the server. */
        int stream = atoi(argv[2]);
        char byte;
        ssize_t amount;
        do amount = recv(stream, &byte, 1u, 0); while (amount < 0 && errno == EINTR);
        FILE *note = fopen(argv[3], "w");
        if (note) { fprintf(note, "revoked %zd\n", amount); fclose(note); }
        return 0;
    }
    if (!strcmp(mode, "cat")) {
        char block[256];
        ssize_t amount;
        while ((amount = read(STDIN_FILENO, block, sizeof(block))) > 0)
            if (write(STDOUT_FILENO, block, (size_t)amount) != amount) return 5;
        return 0;
    }
    fprintf(stderr, "usage: exec-probe MODE ...\n");
    return 64;
}
