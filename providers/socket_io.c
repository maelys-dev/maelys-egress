#include "providers/socket_io.h"
#include "maelys/sys/fdpass.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/socket.h>

int egress_tls_socket_init(egress_tls_socket_t *socket, int fd) {
    if (!socket) { errno = EINVAL; return -1; }
    *socket = (egress_tls_socket_t){.fd = -1, .failed = 1};
    int flags = fcntl(fd, F_GETFL);
    int type = 0;
    socklen_t type_length = (socklen_t)sizeof(type);
    struct sockaddr_storage address = {0};
    socklen_t address_length = (socklen_t)sizeof(address);
    if (flags < 0 || !(flags & O_NONBLOCK) ||
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length) != 0 ||
        type != SOCK_STREAM ||
        getsockname(fd, (struct sockaddr *)&address, &address_length) != 0) {
        errno = EINVAL;
        return -1;
    }
    *socket = (egress_tls_socket_t){.fd = fd,
        .unix_stream = address.ss_family == AF_UNIX};
    return 0;
}

ssize_t egress_tls_socket_receive(
    egress_tls_socket_t *socket, void *buffer, size_t capacity) {
    if (!socket || !buffer || !capacity || capacity > (size_t)SSIZE_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (socket->failed) { errno = EPROTO; return -1; }
    if (!socket->unix_stream) {
        /* SCM_RIGHTS belongs to AF_UNIX on Linux/macOS. Other stream
         * families retain the byte transport promised by the TLS seam. */
        return recv(socket->fd, buffer, capacity, 0);
    }
    size_t received = 0, fd_count = 0;
    unsigned flags = 0;
    maelys_sys_result_t result = maelys_sys_fd_stream_receive(
        socket->fd, buffer, capacity, &received, NULL, 0, &fd_count, &flags);
    if (result == MAELYS_SYS_OK) {
        /* System closes all received descriptors (capacity zero). TLS has
         * no use for rights or other ancillary messages: reject rather than
         * passing their accompanying bytes to the TLS decoder. In particular,
         * macOS control-only delivery is neither EOF nor WANT_READ. */
        if (flags != 0 || received == 0) {
            socket->failed = 1;
            errno = EPROTO;
            return -1;
        }
        return (ssize_t)received;
    }
    if (result == MAELYS_SYS_ERR_CLOSED) return 0;
    if (result == MAELYS_SYS_ERR_WOULD_BLOCK) errno = EAGAIN;
    else if (result == MAELYS_SYS_ERR_RESET) errno = ECONNRESET;
    else if (result != MAELYS_SYS_ERR_OS) errno = EINVAL;
    return -1;
}
