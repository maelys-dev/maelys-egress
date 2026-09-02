#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "src/server_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Listening sockets: loopback/remote TCP, private AF_UNIX with identity
 * verification, and the private TCP pair backing native connector streams. */

int egress_listener_is_loopback_host(const char *host) {
    return strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0;
}

int egress_listener_create_tcp(const maelys_egress_config_t *config, uint16_t *out_port) {
    char service[6];
    (void)snprintf(service, sizeof(service), "%u", (unsigned int)config->port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(config->listen_host, service, &hints, &addresses) != 0) return -1;
    int listener = -1;
    int saved = EADDRNOTAVAIL;
    for (const struct addrinfo *address = addresses; address; address = address->ai_next) {
        listener = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (listener < 0) { saved = errno; continue; }
        int enabled = 1;
        (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (maelys_sys_fd_set_cloexec(listener) != MAELYS_SYS_OK ||
            maelys_sys_fd_set_nonblocking(listener) != MAELYS_SYS_OK ||
            bind(listener, address->ai_addr, address->ai_addrlen) != 0 ||
            listen(listener, 128) != 0) {
            saved = errno;
            (void)maelys_sys_fd_close(&listener);
            continue;
        }
        break;
    }
    freeaddrinfo(addresses);
    if (listener < 0) { errno = saved; return -1; }
    struct sockaddr_storage bound;
    socklen_t bound_length = sizeof(bound);
    if (getsockname(listener, (struct sockaddr *)&bound, &bound_length) != 0) {
        saved = errno;
        (void)maelys_sys_fd_close(&listener);
        errno = saved;
        return -1;
    }
    if (bound.ss_family == AF_INET) {
        *out_port = ntohs(((struct sockaddr_in *)&bound)->sin_port);
    } else if (bound.ss_family == AF_INET6) {
        *out_port = ntohs(((struct sockaddr_in6 *)&bound)->sin6_port);
    } else {
        (void)maelys_sys_fd_close(&listener);
        errno = EAFNOSUPPORT;
        return -1;
    }
    return listener;
}

void egress_listener_unlink_unix_identity(const char *path, dev_t device, ino_t inode) {
    struct stat current;
    if (path && path[0] && lstat(path, &current) == 0 &&
        S_ISSOCK(current.st_mode) && current.st_dev == device &&
        current.st_ino == inode) {
        (void)unlink(path);
    }
}

int egress_listener_create_unix(
    const maelys_egress_config_t *config,
    dev_t *out_device,
    ino_t *out_inode) {
    struct stat existing;
    if (lstat(config->unix_path, &existing) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) return -1;
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) return -1;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t path_length = strlen(config->unix_path);
    memcpy(address.sun_path, config->unix_path, path_length + 1u);
    socklen_t address_length = (socklen_t)(
        offsetof(struct sockaddr_un, sun_path) + path_length + 1u);
    if (maelys_sys_fd_set_cloexec(listener) != MAELYS_SYS_OK ||
        maelys_sys_fd_set_nonblocking(listener) != MAELYS_SYS_OK ||
        bind(listener, (struct sockaddr *)&address, address_length) != 0) {
        int saved = errno;
        (void)maelys_sys_fd_close(&listener);
        errno = saved;
        return -1;
    }
    struct stat created;
    if (lstat(config->unix_path, &created) != 0 || !S_ISSOCK(created.st_mode)) {
        int saved = errno ? errno : EIO;
        (void)maelys_sys_fd_close(&listener);
        errno = saved;
        return -1;
    }
    if (chmod(config->unix_path, S_IRUSR | S_IWUSR) != 0 ||
        listen(listener, 128) != 0) {
        int saved = errno;
        (void)maelys_sys_fd_close(&listener);
        egress_listener_unlink_unix_identity(config->unix_path, created.st_dev, created.st_ino);
        errno = saved;
        return -1;
    }
    *out_device = created.st_dev;
    *out_inode = created.st_ino;
    return listener;
}

int egress_listener_unix_peer_allowed(const maelys_egress_server_t *server, int client) {
    if (!server->config.listen_unix ||
        server->config.unix_peer_policy == MAELYS_EGRESS_UNIX_PEER_AUTHENTICATED) return 1;
#if defined(__linux__) && defined(SO_PEERCRED)
    struct ucred credentials;
    socklen_t length = sizeof(credentials);
    return getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 &&
           credentials.uid == geteuid();
#elif defined(__APPLE__)
    uid_t effective_uid = (uid_t)-1;
    gid_t effective_gid = (gid_t)-1;
    return getpeereid(client, &effective_uid, &effective_gid) == 0 &&
           effective_uid == geteuid();
#else
    (void)client;
    return 0;
#endif
}

int egress_listener_create_private_tcp_pair(int *out_server, int *out_client) {
    *out_server = -1;
    *out_client = -1;
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) return 0;
    if (maelys_sys_fd_set_cloexec(listener) != MAELYS_SYS_OK) {
        (void)maelys_sys_fd_close(&listener);
        return 0;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        (void)maelys_sys_fd_close(&listener);
        return 0;
    }
    socklen_t address_length = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &address_length) != 0) {
        (void)maelys_sys_fd_close(&listener);
        return 0;
    }
    int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client < 0 || maelys_sys_fd_set_cloexec(client) != MAELYS_SYS_OK ||
        connect(client, (const struct sockaddr *)&address, address_length) != 0) {
        (void)maelys_sys_fd_close(&client);
        (void)maelys_sys_fd_close(&listener);
        return 0;
    }
    int server_fd;
    do {
        server_fd = accept(listener, NULL, NULL);
    } while (server_fd < 0 && errno == EINTR);
    (void)maelys_sys_fd_close(&listener);
    if (server_fd < 0 || maelys_sys_fd_set_cloexec(server_fd) != MAELYS_SYS_OK ||
        maelys_sys_fd_set_nonblocking(server_fd) != MAELYS_SYS_OK) {
        (void)maelys_sys_fd_close(&server_fd);
        (void)maelys_sys_fd_close(&client);
        return 0;
    }
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    (void)setsockopt(server_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
    (void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    *out_server = server_fd;
    *out_client = client;
    return 1;
}
