/* SPDX-License-Identifier: MPL-2.0 */
/* No Egress core or proxy credentials: link libmaelys_egress_client alone. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include <maelys/egress_client.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void require(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "native example: %s\n", message); exit(1); }
}

static void deployment_checks(void) {
    require(geteuid() == 10002, "unexpected application UID");
    struct ifaddrs *interfaces = NULL;
    require(getifaddrs(&interfaces) == 0, "cannot inspect interfaces");
    for (struct ifaddrs *entry = interfaces; entry; entry = entry->ifa_next) {
        if (entry->ifa_flags & IFF_LOOPBACK) continue;
        /* Some Linux kernels precreate dormant tunnel devices in every netns.
         * They must be down and unaddressed; the app has no NET_ADMIN to
         * configure them. Mere device existence does not confer a route. */
        require((entry->ifa_flags & IFF_UP) == 0, "non-loopback interface is up");
        require(!entry->ifa_addr || (entry->ifa_addr->sa_family != AF_INET &&
            entry->ifa_addr->sa_family != AF_INET6), "non-loopback IP address is present");
    }
    freeifaddrs(interfaces);
    const char *proxy[] = {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"};
    for (size_t i = 0; i < sizeof(proxy) / sizeof(proxy[0]); ++i) {
        const char *value = getenv(proxy[i]);
        require(!value || !*value, "proxy credentials/environment must not reach this application");
    }
    require(access("/usr/bin/maelys-egress", F_OK) != 0 && errno == ENOENT, "Egress CLI leaked into app");
    require(access("/run/maelys-config", F_OK) != 0 && errno == ENOENT, "broker configuration is mounted");
    struct stat parent;
    require(stat("/run/maelys-egress", &parent) == 0 && S_ISDIR(parent.st_mode) &&
        parent.st_uid == 10001 && parent.st_gid == 20000 && (parent.st_mode & 07777) == 02750,
        "socket capability directory has wrong identity or mode");
    int fd = open("/run/maelys-egress/forbidden-write", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) { close(fd); require(0, "application can write the capability directory"); }
    require(errno == EROFS || errno == EACCES, "unexpected capability-directory write result");
    puts("{\"check\":\"deployment\",\"ok\":true}");
}

static maelys_egress_client_channel_t *open_channel(void) {
    maelys_egress_client_channel_t *channel = NULL;
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        maelys_egress_client_result_t result = maelys_egress_client_channel_open(
            "/run/maelys-egress/channel.sock", 1000u, &channel, NULL);
        if (result == MAELYS_EGRESS_CLIENT_OK) return channel;
        require(result == MAELYS_EGRESS_CLIENT_ERR_IO || result == MAELYS_EGRESS_CLIENT_ERR_UNANSWERED ||
            result == MAELYS_EGRESS_CLIENT_ERR_BUSY, "bootstrap was refused or malformed");
        struct timespec delay = {.tv_nsec = 100000000L};
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
    }
    require(0, "broker readiness deadline expired");
    return NULL;
}

static void exchange(int fd, char out_address[INET_ADDRSTRLEN]) {
    static const char request[] = "native-compose\n";
    size_t sent = 0;
    while (sent < sizeof(request) - 1u) {
        ssize_t count = send(fd, request + sent, sizeof(request) - 1u - sent, 0);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "mediated write failed");
        sent += (size_t)count;
    }
    char response[64] = {0}; size_t received = 0;
    while (received + 1u < sizeof(response)) {
        ssize_t count = recv(fd, response + received, 1u, 0);
        if (count < 0 && errno == EINTR) continue;
        require(count == 1, "mediated response was not complete");
        if (response[received++] == '\n') break;
    }
    static const char prefix[] = "native-compose ";
    require(received > sizeof(prefix) && response[received - 1u] == '\n' &&
        memcmp(response, prefix, sizeof(prefix) - 1u) == 0, "wrong upstream response");
    response[received - 1u] = '\0';
    const char *address = response + sizeof(prefix) - 1u;
    struct in_addr parsed;
    require(inet_pton(AF_INET, address, &parsed) == 1, "upstream did not return its numeric IP");
    require(strlen(address) < INET_ADDRSTRLEN, "upstream address too long");
    strcpy(out_address, address);
}

static void direct_must_fail(const char *address) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0 && fcntl(fd, F_SETFL, O_NONBLOCK) == 0, "cannot make direct network probe");
    struct sockaddr_in target = {.sin_family = AF_INET, .sin_port = htons(8080)};
    require(inet_pton(AF_INET, address, &target.sin_addr) == 1, "invalid direct target");
    int connected = connect(fd, (struct sockaddr *)&target, sizeof(target));
    int reason = connected == 0 ? 0 : errno;
    if (reason == EINPROGRESS) {
        struct pollfd item = {.fd = fd, .events = POLLOUT};
        require(poll(&item, 1u, 2000) == 1, "direct probe timed out instead of proving network isolation");
        socklen_t length = sizeof(reason);
        require(getsockopt(fd, SOL_SOCKET, SO_ERROR, &reason, &length) == 0, "direct probe status failed");
    }
    close(fd);
    require(reason == ENETUNREACH || reason == EHOSTUNREACH,
        "direct access was not rejected as unreachable by the network namespace");
    printf("{\"check\":\"direct-denied\",\"ok\":true,\"address\":\"%s\",\"port\":8080}\n", address);
}

int main(void) {
    (void)signal(SIGPIPE, SIG_IGN);
    (void)alarm(30); /* Whole example bound, including any accidental blocking I/O. */
    deployment_checks();
    maelys_egress_client_channel_t *channel = open_channel();
    uint64_t timeout = maelys_egress_client_channel_connect_timeout_ms(channel) + 1000u;
    int stream = -1;
    maelys_egress_client_result_t result = maelys_egress_client_connect(
        maelys_egress_client_channel_fd(channel), "upstream", 8080, timeout, &stream, NULL);
    require(result == MAELYS_EGRESS_CLIENT_OK, "allowlisted destination was not opened");
    struct timeval io_timeout = {.tv_sec = 3};
    require(setsockopt(stream, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout)) == 0 &&
        setsockopt(stream, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout)) == 0,
        "cannot bound mediated I/O");
    char address[INET_ADDRSTRLEN];
    exchange(stream, address);
    puts("{\"check\":\"native-allowed\",\"ok\":true}");
    direct_must_fail(address); /* Same live destination, learned without app-side DNS. */
    int forbidden = -1;
    result = maelys_egress_client_connect(maelys_egress_client_channel_fd(channel),
        "upstream", 8081, timeout, &forbidden, NULL);
    require(result == MAELYS_EGRESS_CLIENT_ERR_DENIED && forbidden == -1,
        "non-allowlisted destination did not return the policy DENIED result");
    puts("{\"check\":\"policy-denied\",\"ok\":true}");
    /* Closing the lease forbids further requests but does not revoke a stream
     * already returned. Keep the stream mediated until its own EOF. */
    maelys_egress_client_channel_close(channel);
    /* This example configures exactly one broker slot. Reacquiring it proves
     * the first channel's asynchronous destruction has actually completed;
     * the following I/O cannot win a race against delayed stream revocation. */
    channel = open_channel();
    maelys_egress_client_channel_close(channel);
    char again[INET_ADDRSTRLEN];
    exchange(stream, again);
    require(strcmp(address, again) == 0, "existing stream changed destination");
    puts("{\"check\":\"lease-independent-stream\",\"ok\":true}");
    require(shutdown(stream, SHUT_WR) == 0, "cannot half-close stream");
    char end;
    require(recv(stream, &end, 1u, 0) == 0, "stream did not close cleanly");
    close(stream);
    puts("{\"check\":\"complete\",\"ok\":true}");
    return 0;
}
