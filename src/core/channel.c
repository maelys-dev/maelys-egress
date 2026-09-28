/*
 * The codec of the mediated-connection channel. Bytes in, fields out, and
 * back; nothing here touches a descriptor. It is compiled into the server
 * library and into the client archive, so it includes the public header and
 * the C library only: a dependency added here would reach a confined process.
 */
#include "maelys/egress_channel.h"

#include <string.h>

static void write_u16(unsigned char *bytes, uint16_t value) {
    bytes[0] = (unsigned char)(value >> 8u);
    bytes[1] = (unsigned char)value;
}

static void write_u32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value >> 24u);
    bytes[1] = (unsigned char)(value >> 16u);
    bytes[2] = (unsigned char)(value >> 8u);
    bytes[3] = (unsigned char)value;
}

static uint16_t read_u16(const unsigned char *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static uint32_t read_u32(const unsigned char *bytes) {
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

/* Printable ASCII only: no NUL, no space, no control byte, nothing above
 * 0x7e. The canonical form of a host is judged by the server, not here. */
static int host_bytes_valid(const char *host, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)host[i];
        if (byte < 0x21u || byte > 0x7eu) return 0;
    }
    return 1;
}

static maelys_egress_channel_status_t refuse(
    const char **out_reason, maelys_egress_channel_status_t status, const char *reason) {
    if (out_reason) *out_reason = reason;
    return status;
}

size_t maelys_egress_channel_encode_request(
    const char *host, uint16_t port, unsigned char *out, size_t capacity) {
    if (!host || !out || port == 0u) return 0u;
    size_t length = strlen(host);
    if (length == 0u || length > MAELYS_EGRESS_CHANNEL_MAX_HOST ||
        !host_bytes_valid(host, length)) {
        return 0u;
    }
    size_t total = MAELYS_EGRESS_CHANNEL_REQUEST_HEADER_SIZE + length;
    if (capacity < total) return 0u;
    write_u32(out, MAELYS_EGRESS_CHANNEL_REQUEST_MAGIC);
    out[4] = (unsigned char)MAELYS_EGRESS_CHANNEL_PROTOCOL_VERSION;
    out[5] = (unsigned char)MAELYS_EGRESS_CHANNEL_PROTOCOL_TCP;
    write_u16(out + 6, port);
    write_u16(out + 8, (uint16_t)length);
    memcpy(out + MAELYS_EGRESS_CHANNEL_REQUEST_HEADER_SIZE, host, length);
    return total;
}

maelys_egress_channel_status_t maelys_egress_channel_decode_request(
    const unsigned char *bytes,
    size_t length,
    maelys_egress_channel_request_t *out,
    const char **out_reason) {
    if (out_reason) *out_reason = NULL;
    if (!bytes || !out) {
        return refuse(out_reason, MAELYS_EGRESS_CHANNEL_MALFORMED,
                      "request decoder needs bytes and an output");
    }
    memset(out, 0, sizeof(*out));
    if (length < MAELYS_EGRESS_CHANNEL_REQUEST_HEADER_SIZE) {
        return refuse(out_reason, MAELYS_EGRESS_CHANNEL_MALFORMED,
                      "datagram shorter than the request header");
    }
    if (read_u32(bytes) != MAELYS_EGRESS_CHANNEL_REQUEST_MAGIC) {
        return refuse(out_reason, MAELYS_EGRESS_CHANNEL_MALFORMED,
                      "request magic is not MECQ");
    }
    out->version = bytes[4];
    out->protocol = bytes[5];
    if (out->version != MAELYS_EGRESS_CHANNEL_PROTOCOL_VERSION) {
        return refuse(out_reason, MAELYS_EGRESS_CHANNEL_UNSUPPORTED,
                      "request version is not 1");
    }
    if (out->protocol != MAELYS_EGRESS_CHANNEL_PROTOCOL_TCP) {
        return refuse(out_reason, MAELYS_EGRESS_CHANNEL_UNSUPPORTED,
                      "request protocol is not TCP");
    }
    out->port = read_u16(bytes + 6);
    out->host_length = read_u16(bytes + 8);
    if (out->port == 0u) {
        return refuse(out_reason, MAELYS_EGRESS_CHANNEL_MALFORMED, "port is zero");
    }
    if (out->host_length == 0u) {
        return refuse(out_reason, MAELYS_EGRESS_CHANNEL_MALFORMED, "host is empty");
    }
    if (out->host_length > MAELYS_EGRESS_CHANNEL_MAX_HOST) {
        return refuse(out_reason, MAELYS_EGRESS_CHANNEL_MALFORMED,
                      "host exceeds 253 bytes");
    }
    if (length != MAELYS_EGRESS_CHANNEL_REQUEST_HEADER_SIZE + out->host_length) {
        return refuse(out_reason, MAELYS_EGRESS_CHANNEL_MALFORMED,
                      "datagram length is not 10 + host_length");
    }
    const char *host = (const char *)bytes + MAELYS_EGRESS_CHANNEL_REQUEST_HEADER_SIZE;
    if (!host_bytes_valid(host, out->host_length)) {
        return refuse(out_reason, MAELYS_EGRESS_CHANNEL_MALFORMED,
                      "host byte outside printable ASCII");
    }
    memcpy(out->host, host, out->host_length);
    out->host[out->host_length] = '\0';
    return MAELYS_EGRESS_CHANNEL_OK;
}

size_t maelys_egress_channel_encode_response(
    maelys_egress_channel_status_t status, unsigned char *out, size_t capacity) {
    if (!out || capacity < MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE ||
        (unsigned int)status > 0xffu) {
        return 0u;
    }
    write_u32(out, MAELYS_EGRESS_CHANNEL_RESPONSE_MAGIC);
    out[4] = (unsigned char)MAELYS_EGRESS_CHANNEL_PROTOCOL_VERSION;
    out[5] = (unsigned char)status;
    out[6] = 0u;
    out[7] = 0u;
    return MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE;
}

int maelys_egress_channel_decode_response(
    const unsigned char *bytes,
    size_t length,
    maelys_egress_channel_response_t *out,
    const char **out_reason) {
    if (out_reason) *out_reason = NULL;
    if (!bytes || !out) {
        (void)refuse(out_reason, MAELYS_EGRESS_CHANNEL_MALFORMED,
                     "response decoder needs bytes and an output");
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (length != MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE) {
        (void)refuse(out_reason, MAELYS_EGRESS_CHANNEL_MALFORMED,
                     "response is not 8 bytes");
        return 0;
    }
    if (read_u32(bytes) != MAELYS_EGRESS_CHANNEL_RESPONSE_MAGIC) {
        (void)refuse(out_reason, MAELYS_EGRESS_CHANNEL_MALFORMED,
                     "response magic is not MECP");
        return 0;
    }
    out->version = bytes[4];
    out->status = bytes[5];
    out->reserved = read_u16(bytes + 6);
    return 1;
}

const char *maelys_egress_channel_status_string(maelys_egress_channel_status_t status) {
    switch (status) {
    case MAELYS_EGRESS_CHANNEL_OK: return "ok";
    case MAELYS_EGRESS_CHANNEL_DENIED: return "denied";
    case MAELYS_EGRESS_CHANNEL_TIMEOUT: return "timeout";
    case MAELYS_EGRESS_CHANNEL_CANCELLED: return "cancelled";
    case MAELYS_EGRESS_CHANNEL_MALFORMED: return "malformed";
    case MAELYS_EGRESS_CHANNEL_UNSUPPORTED: return "unsupported";
    case MAELYS_EGRESS_CHANNEL_RESOURCE: return "resource";
    case MAELYS_EGRESS_CHANNEL_INTERNAL: return "internal";
    }
    return "unknown";
}
