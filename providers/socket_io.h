#ifndef EGRESS_PROVIDER_SOCKET_IO_H
#define EGRESS_PROVIDER_SOCKET_IO_H

#include <stddef.h>
#include <sys/types.h>

/* A borrowed connected stream, never closed or made blocking here. The
 * transport classification is fixed at creation, not guessed after an I/O
 * error. Protocol violations poison the session, including later retries. */
typedef struct egress_tls_socket {
    int fd;
    int unix_stream;
    int failed;
} egress_tls_socket_t;

int egress_tls_socket_init(egress_tls_socket_t *socket, int fd);
ssize_t egress_tls_socket_receive(
    egress_tls_socket_t *socket, void *buffer, size_t capacity);

#endif
