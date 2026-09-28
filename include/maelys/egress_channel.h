#ifndef MAELYS_EGRESS_CHANNEL_H
#define MAELYS_EGRESS_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The wire of the mediated-connection channel, version 1: what a confined
 * process sends to ask for one TCP destination, and what the broker answers
 * before attaching the stream as ancillary data. This header names bytes
 * only — no descriptor, no socket, no policy — and its codec is compiled into
 * both the server library and the client archive, so the two sides can never
 * disagree on a field. The contract is proposals/egress-channel-v1.md until
 * v1 freezes; it then moves under protocol/.
 *
 * Integers are big-endian. A request is a 10-byte header followed by the
 * host; a response is 8 bytes. Both travel as one datagram each.
 */
#define MAELYS_EGRESS_CHANNEL_PROTOCOL_VERSION 1u
#define MAELYS_EGRESS_CHANNEL_REQUEST_MAGIC UINT32_C(0x4d454351)  /* "MECQ" */
#define MAELYS_EGRESS_CHANNEL_RESPONSE_MAGIC UINT32_C(0x4d454350) /* "MECP" */
#define MAELYS_EGRESS_CHANNEL_PROTOCOL_TCP 1u
#define MAELYS_EGRESS_CHANNEL_MAX_HOST 253u
#define MAELYS_EGRESS_CHANNEL_REQUEST_HEADER_SIZE 10u
#define MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE \
    (MAELYS_EGRESS_CHANNEL_REQUEST_HEADER_SIZE + MAELYS_EGRESS_CHANNEL_MAX_HOST)
#define MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE 8u

/*
 * The status byte of a response. These are the protocol's codes, mapped by
 * an explicit table from whatever the server observes; they are not a mirror
 * of maelys_egress_result_t, so the C API and the wire can move separately.
 * A client treats a code it does not know as INTERNAL.
 */
typedef enum maelys_egress_channel_status {
    MAELYS_EGRESS_CHANNEL_OK = 0,          /* the stream is attached */
    MAELYS_EGRESS_CHANNEL_DENIED = 1,      /* the sealed policy refuses it */
    MAELYS_EGRESS_CHANNEL_TIMEOUT = 2,     /* upstream connect past the deadline */
    MAELYS_EGRESS_CHANNEL_CANCELLED = 3,   /* the server is stopping */
    MAELYS_EGRESS_CHANNEL_MALFORMED = 4,   /* the request violates the contract */
    MAELYS_EGRESS_CHANNEL_UNSUPPORTED = 5, /* version or protocol not spoken */
    MAELYS_EGRESS_CHANNEL_RESOURCE = 6,    /* the server could not allocate */
    MAELYS_EGRESS_CHANNEL_INTERNAL = 7     /* anything else; see the server log */
} maelys_egress_channel_status_t;

typedef struct maelys_egress_channel_request {
    uint8_t version;
    uint8_t protocol;
    uint16_t port;
    uint16_t host_length;
    /* The host bytes as sent, NUL-terminated for convenience. The codec
     * checks their shape (1..253 bytes, each printable ASCII 0x21..0x7e);
     * whether they form a canonical host is the server's decision. */
    char host[MAELYS_EGRESS_CHANNEL_MAX_HOST + 1u];
} maelys_egress_channel_request_t;

typedef struct maelys_egress_channel_response {
    uint8_t version;
    uint8_t status;
    uint16_t reserved; /* zero in v1; a v1 client ignores the value */
} maelys_egress_channel_response_t;

/*
 * Write one request. Returns the bytes written, or 0 when nothing valid can
 * be written: a NULL or empty host, a host over 253 bytes or with a byte
 * outside printable ASCII, port 0, or a capacity below the message.
 */
size_t maelys_egress_channel_encode_request(
    const char *host, uint16_t port, unsigned char *out, size_t capacity);

/*
 * Read one request datagram. OK fills out; MALFORMED and UNSUPPORTED are the
 * status the server answers with, and out_reason (may be NULL) then names the
 * rule broken, as a static string. The magic is checked first, then the
 * version, then the protocol, then the fields: a wrong version is
 * UNSUPPORTED whatever follows it.
 */
maelys_egress_channel_status_t maelys_egress_channel_decode_request(
    const unsigned char *bytes,
    size_t length,
    maelys_egress_channel_request_t *out,
    const char **out_reason);

/* Write one response of version 1. Returns 8, or 0 when the status does not
 * fit a byte or the capacity is short. */
size_t maelys_egress_channel_encode_response(
    maelys_egress_channel_status_t status, unsigned char *out, size_t capacity);

/*
 * Read one response datagram. Returns 1 when the size and the magic hold and
 * fills out, whatever the version and status bytes carry — the caller decides
 * what to make of a version it does not speak or a status it does not know.
 * Returns 0 with out_reason set when the datagram is not a response at all.
 */
int maelys_egress_channel_decode_response(
    const unsigned char *bytes,
    size_t length,
    maelys_egress_channel_response_t *out,
    const char **out_reason);

const char *maelys_egress_channel_status_string(
    maelys_egress_channel_status_t status);

#ifdef __cplusplus
}
#endif

#endif
