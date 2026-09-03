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
#include <time.h>
#include <unistd.h>

/* Listening sockets: loopback/remote TCP, private AF_UNIX with identity
 * verification, and the private TCP pair backing native connector streams. */

int egress_listener_is_loopback_host(const char *host) {
    return strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0;
}

void egress_socket_release(maelys_sys_socket_t **socket_handle, int *fd_view) {
    (void)maelys_sys_socket_release(socket_handle);
    if (fd_view) *fd_view = -1;
}

maelys_sys_socket_t *egress_listener_create_tcp(
    const maelys_egress_config_t *config, uint16_t *out_port) {
    char service[6];
    (void)snprintf(service, sizeof(service), "%u", (unsigned int)config->port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(config->listen_host, service, &hints, &addresses) != 0) return NULL;
    maelys_sys_socket_t *listener = NULL;
    int saved = EADDRNOTAVAIL;
    for (const struct addrinfo *address = addresses; address; address = address->ai_next) {
        if (maelys_sys_socket_create(address->ai_family, address->ai_socktype,
                                     address->ai_protocol, &listener) != MAELYS_SYS_OK) {
            saved = errno;
            continue;
        }
        /* System exposes no socket options; the listener needs address reuse
         * so a restart does not wait out TIME_WAIT. */
        int enabled = 1;
        (void)setsockopt(maelys_sys_socket_native_fd(listener), SOL_SOCKET,
                         SO_REUSEADDR, &enabled, sizeof(enabled));
        if (maelys_sys_socket_bind(listener, address->ai_addr,
                                   address->ai_addrlen) != MAELYS_SYS_OK ||
            maelys_sys_socket_listen(listener, 128) != MAELYS_SYS_OK) {
            saved = errno;
            (void)maelys_sys_socket_release(&listener);
            continue;
        }
        break;
    }
    freeaddrinfo(addresses);
    if (!listener) { errno = saved; return NULL; }
    struct sockaddr_storage bound;
    socklen_t bound_length = sizeof(bound);
    if (getsockname(maelys_sys_socket_native_fd(listener),
                    (struct sockaddr *)&bound, &bound_length) != 0) {
        saved = errno;
        (void)maelys_sys_socket_release(&listener);
        errno = saved;
        return NULL;
    }
    if (bound.ss_family == AF_INET) {
        *out_port = ntohs(((struct sockaddr_in *)&bound)->sin_port);
    } else if (bound.ss_family == AF_INET6) {
        *out_port = ntohs(((struct sockaddr_in6 *)&bound)->sin6_port);
    } else {
        (void)maelys_sys_socket_release(&listener);
        errno = EAFNOSUPPORT;
        return NULL;
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

maelys_sys_socket_t *egress_listener_create_unix(
    const maelys_egress_config_t *config,
    dev_t *out_device,
    ino_t *out_inode) {
    struct stat existing;
    if (lstat(config->unix_path, &existing) == 0) {
        errno = EEXIST;
        return NULL;
    }
    if (errno != ENOENT) return NULL;
    maelys_sys_socket_t *listener = NULL;
    if (maelys_sys_socket_create(AF_UNIX, SOCK_STREAM, 0, &listener) != MAELYS_SYS_OK) {
        return NULL;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t path_length = strlen(config->unix_path);
    memcpy(address.sun_path, config->unix_path, path_length + 1u);
    socklen_t address_length = (socklen_t)(
        offsetof(struct sockaddr_un, sun_path) + path_length + 1u);
    if (maelys_sys_socket_bind(listener, (struct sockaddr *)&address,
                               address_length) != MAELYS_SYS_OK) {
        int saved = errno;
        (void)maelys_sys_socket_release(&listener);
        errno = saved;
        return NULL;
    }
    struct stat created;
    if (lstat(config->unix_path, &created) != 0 || !S_ISSOCK(created.st_mode)) {
        int saved = errno ? errno : EIO;
        (void)maelys_sys_socket_release(&listener);
        errno = saved;
        return NULL;
    }
    if (chmod(config->unix_path, S_IRUSR | S_IWUSR) != 0 ||
        maelys_sys_socket_listen(listener, 128) != MAELYS_SYS_OK) {
        int saved = errno;
        (void)maelys_sys_socket_release(&listener);
        egress_listener_unlink_unix_identity(config->unix_path, created.st_dev, created.st_ino);
        errno = saved;
        return NULL;
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

int egress_listener_create_private_tcp_pair(
    maelys_sys_socket_t **out_server, int *out_client) {
    *out_server = NULL;
    *out_client = -1;
    maelys_sys_socket_t *listener = NULL;
    if (maelys_sys_socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &listener) !=
        MAELYS_SYS_OK) return 0;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t address_length = sizeof(address);
    if (maelys_sys_socket_bind(listener, (const struct sockaddr *)&address,
                               sizeof(address)) != MAELYS_SYS_OK ||
        maelys_sys_socket_listen(listener, 1) != MAELYS_SYS_OK ||
        getsockname(maelys_sys_socket_native_fd(listener),
                    (struct sockaddr *)&address, &address_length) != 0) {
        (void)maelys_sys_socket_release(&listener);
        return 0;
    }
    /* The embedder receives a blocking TCP descriptor it owns outright, so
     * this end is created bare: a System handle cannot give up its
     * descriptor. Its peer, relayed by Egress, is accepted through System. */
    int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client < 0 || maelys_sys_fd_set_cloexec(client) != MAELYS_SYS_OK ||
        connect(client, (const struct sockaddr *)&address, address_length) != 0) {
        (void)maelys_sys_fd_close(&client);
        (void)maelys_sys_socket_release(&listener);
        return 0;
    }
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    (void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    maelys_sys_socket_t *server_socket = NULL;
    maelys_sys_result_t accepted = MAELYS_SYS_ERR_OS;
    /* The loopback connect completed, so the peer is queued; the listener is
     * non-blocking and may still report EAGAIN for an instant. */
    for (unsigned attempt = 0u; attempt < 200u; ++attempt) {
        accepted = maelys_sys_socket_accept(listener, NULL, NULL, &server_socket);
        if (accepted == MAELYS_SYS_OK ||
            (errno != EAGAIN && errno != EWOULDBLOCK)) break;
        struct timespec pause = { 0, 1000000L };
        (void)nanosleep(&pause, NULL);
    }
    (void)maelys_sys_socket_release(&listener);
    if (accepted != MAELYS_SYS_OK) {
        (void)maelys_sys_fd_close(&client);
        return 0;
    }
    *out_server = server_socket;
    *out_client = client;
    return 1;
}
