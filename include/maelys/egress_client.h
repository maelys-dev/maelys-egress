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
 * the relayed stream. It lives in libmaelys_egress_client, which links with
 * the C library alone — no Egress core, no maelys-system — so nothing of the
 * proxy reaches the process that is being confined. The contract is
 * proposals/egress-channel-v1.md until v1 freezes.
 */
#define MAELYS_EGRESS_CLIENT_ABI_VERSION 1u

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
    MAELYS_EGRESS_CLIENT_ERR_INTERNAL     /* the server failed for a reason it keeps */
} maelys_egress_client_result_t;

/*
 * Ask the channel for one exact TCP destination and receive the stream.
 *
 * channel_fd is the client end of a channel, owned by the caller throughout.
 * host is the canonical host of a destination the supervisor's policy allows;
 * port is 1..65535. read_timeout_ms bounds the wait for the answer: choose
 * at least the server's connect deadline, which the supervisor knows; 0 waits
 * without bound. When the deadline passes, the channel is shut down and
 * ERR_UNANSWERED is returned: with one request in flight and no identifier,
 * a late answer would be paired with the wrong request, so a further call
 * on that channel fails with ERR_IO; the caller still closes the descriptor.
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

#ifdef __cplusplus
}
#endif

#endif
