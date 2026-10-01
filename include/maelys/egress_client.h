/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MAELYS_EGRESS_CLIENT_H
#define MAELYS_EGRESS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The client of the mediated-connection channel, for the confined process:
 * it sends one request on the channel its supervisor handed it and receives
 * the relayed stream. It lives in libmaelys_egress_client, which holds no
 * Egress core and, of maelys-system, only the object that passes a
 * descriptor — so nothing of the proxy, the loop or the threads reaches the
 * process that is being confined, and the archive links alone, without
 * -pthread. The contract is protocol/egress-channel-v1.md.
 */
/*
 * Two numbers, as in <maelys/egress.h>: MAELYS_EGRESS_CLIENT_ABI_VERSION is
 * the revision of this interface and rises with every change of this header
 * or of <maelys/egress_channel.h>; MAELYS_EGRESS_CLIENT_ABI_COMPATIBLE_SINCE
 * is the oldest revision whose declarations all still hold, and rises only
 * on a break. A consumer written for revision N is served when
 * COMPATIBLE_SINCE <= N <= ABI_VERSION. Revision 1 is 0.22.0; 0.23.0 added
 * the bootstrap and ERR_BUSY under the same number; revision 2 is that
 * interface. tests/public/abi-client-1.h holds the floor.
 */
#define MAELYS_EGRESS_CLIENT_ABI_VERSION 2u
#define MAELYS_EGRESS_CLIENT_ABI_COMPATIBLE_SINCE 1u

/*
 * One enumeration serves both calls of this header, and it may gain
 * enumerators in a later release; existing enumerators keep their values. A
 * switch over it needs a default, and that default treats an unknown result
 * as a failure: no stream, no channel, never success. On any result other
 * than OK the output is -1 or NULL and nothing received stays open.
 *
 * Each call returns its own subset:
 *
 *   maelys_egress_client_connect       OK, ARGUMENT, IO, UNANSWERED, PROTOCOL,
 *                                      DENIED, TIMEOUT, CANCELLED, UNSUPPORTED,
 *                                      RESOURCE, INTERNAL. Never BUSY.
 *   maelys_egress_client_channel_open  OK, ARGUMENT, IO, UNANSWERED, PROTOCOL,
 *                                      DENIED, BUSY, CANCELLED, UNSUPPORTED,
 *                                      RESOURCE, INTERNAL. Never TIMEOUT.
 *
 * A status the peer sends and this client does not know is INTERNAL in both.
 * ERR_BUSY was added in 0.23.0, after ERR_INTERNAL, with the bootstrap.
 */
typedef enum maelys_egress_client_result {
    MAELYS_EGRESS_CLIENT_OK = 0,
    /* Local refusals, before or after the wire. */
    MAELYS_EGRESS_CLIENT_ERR_ARGUMENT,    /* the call itself was wrong */
    MAELYS_EGRESS_CLIENT_ERR_IO,          /* the channel could not carry the exchange */
    MAELYS_EGRESS_CLIENT_ERR_UNANSWERED,  /* the read deadline passed; the channel is shut */
    MAELYS_EGRESS_CLIENT_ERR_PROTOCOL,    /* the server broke the contract */
    /* The server's answer, one per status of the protocol. */
    MAELYS_EGRESS_CLIENT_ERR_DENIED,      /* the sealed policy refuses the destination */
    MAELYS_EGRESS_CLIENT_ERR_TIMEOUT,     /* the upstream connect passed the server's deadline */
    MAELYS_EGRESS_CLIENT_ERR_CANCELLED,   /* the server is stopping */
    MAELYS_EGRESS_CLIENT_ERR_UNSUPPORTED, /* the server does not speak this version */
    MAELYS_EGRESS_CLIENT_ERR_RESOURCE,    /* the server could not allocate */
    MAELYS_EGRESS_CLIENT_ERR_INTERNAL,    /* the server failed for a reason it keeps */
    MAELYS_EGRESS_CLIENT_ERR_BUSY         /* bootstrap pending/active capacity exhausted */
} maelys_egress_client_result_t;

/* Bootstrap v1: protocol/egress-channel-bootstrap-v1.md.
 * Added in 0.23.0 without breaking client revision 1.
 * Open one absolute canonical Unix pathname within one non-zero monotonic
 * deadline (connect + write + receive). ERR_UNANSWERED is a local deadline;
 * ERR_BUSY is a broker response, never the channel's ERR_TIMEOUT.
 * On failure *out_channel is NULL and every received descriptor is closed.
 * The opaque handle owns BOTH the lifetime lease and the CLOEXEC datagram
 * channel. Retain it while using its borrowed fd with client_connect; do not
 * close that fd separately or use it after channel_close. Operations on one
 * handle must be externally serialized, including close. Closing the handle
 * does not revoke destination streams already returned by client_connect.
 * No descriptor number is assigned and no secret is sent by the client. */
typedef struct maelys_egress_client_channel maelys_egress_client_channel_t;
maelys_egress_client_result_t maelys_egress_client_channel_open(
    const char *absolute_path, uint64_t open_timeout_ms,
    maelys_egress_client_channel_t **out_channel, char **out_error);
int maelys_egress_client_channel_fd(const maelys_egress_client_channel_t *channel);
uint64_t maelys_egress_client_channel_connect_timeout_ms(
    const maelys_egress_client_channel_t *channel);
void maelys_egress_client_channel_close(maelys_egress_client_channel_t *channel);

/*
 * Ask the channel for one exact TCP destination and receive the stream.
 *
 * channel_fd is the client end of a channel, owned by the caller throughout.
 * host is the canonical host of a destination the supervisor's policy allows;
 * port is 1..65535. read_timeout_ms bounds the wait for the answer as one
 * monotonic interval: signals do not restart it. Choose at least the server's
 * connect deadline, which the supervisor knows; 0 waits without bound. When
 * the deadline passes, the channel is shut down and ERR_UNANSWERED is
 * returned: with one request in flight and no identifier, a late answer
 * would be paired with the wrong request, so a further call on that channel
 * fails with ERR_IO; the caller still closes the descriptor.
 *
 * On OK, *out_stream_fd is a blocking CLOEXEC TCP stream: Egress's own end
 * of a private relay, never the upstream socket. Its opening says nothing
 * about the bytes that follow: the SNI guard runs on the first of them,
 * quotas on every one, and either closes the stream. The caller owns it.
 *
 * Any other result leaves *out_stream_fd at -1 and every descriptor the
 * server may have sent closed. *out_error (may be NULL) then names the
 * reason; free it with maelys_egress_client_error_free.
 */
maelys_egress_client_result_t maelys_egress_client_connect(
    int channel_fd,
    const char *host,
    uint16_t port,
    uint64_t read_timeout_ms,
    int *out_stream_fd,
    char **out_error);

const char *maelys_egress_client_result_string(maelys_egress_client_result_t result);
void maelys_egress_client_error_free(char *error);
unsigned int maelys_egress_client_abi_version(void);
unsigned int maelys_egress_client_abi_compatible_since(void);

#ifdef __cplusplus
}
#endif

#endif
