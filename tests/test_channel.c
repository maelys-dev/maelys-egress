/*
 * The conformance vectors of the channel, replayed against the codec, plus
 * the encoder's own refusals. This binary links the codec object alone —
 * never the library — so a dependency creeping into the codec fails here
 * before it reaches a confined process.
 */
#include "maelys/egress_channel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "channel: %s failed at line %d\n", #condition, __LINE__); \
            exit(1); \
        } \
    } while (0)

#define CHECK_VECTOR(condition, name) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "channel: vector %s: %s failed at line %d\n", \
                          name, #condition, __LINE__); \
            exit(1); \
        } \
    } while (0)

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Whitespace inside the hex is for reading; "-" is the empty datagram. */
static size_t parse_hex(const char *text, unsigned char *out, size_t capacity) {
    size_t length = 0u;
    int high = -1;
    for (const char *p = text; *p; ++p) {
        if (*p == ' ' || *p == '-') continue;
        int value = hex_value(*p);
        CHECK(value >= 0);
        if (high < 0) {
            high = value;
        } else {
            CHECK(length < capacity);
            out[length++] = (unsigned char)((high << 4) | value);
            high = -1;
        }
    }
    CHECK(high < 0);
    return length;
}

typedef struct vector {
    char name[128];
    unsigned char bytes[1024];
    size_t length;
    char expected[512];
} vector_t;

static int read_vector(FILE *stream, vector_t *vector) {
    char line[2048];
    while (fgets(line, sizeof(line), stream)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *tab = strchr(line, '\t');
        CHECK(tab != NULL);
        *tab = '\0';
        char *hex = tab + 1;
        tab = strchr(hex, '\t');
        CHECK(tab != NULL);
        *tab = '\0';
        char *expected = tab + 1;
        expected[strcspn(expected, "\n")] = '\0';
        (void)snprintf(vector->name, sizeof(vector->name), "%s", line);
        vector->length = parse_hex(hex, vector->bytes, sizeof(vector->bytes));
        (void)snprintf(vector->expected, sizeof(vector->expected), "%s", expected);
        return 1;
    }
    return 0;
}

static FILE *open_vectors(const char *name) {
    char path[512];
    (void)snprintf(path, sizeof(path), "tests/vectors/channel/%s", name);
    FILE *stream = fopen(path, "r");
    if (!stream) {
        (void)fprintf(stderr, "channel: cannot open %s (run from the repository root)\n", path);
        exit(1);
    }
    return stream;
}

static int replay_requests(void) {
    FILE *stream = open_vectors("requests.txt");
    vector_t vector;
    int count = 0;
    while (read_vector(stream, &vector)) {
        ++count;
        maelys_egress_channel_request_t request;
        const char *reason = (const char *)0x1;
        maelys_egress_channel_status_t status = maelys_egress_channel_decode_request(
            vector.bytes, vector.length, &request, &reason);
        if (strncmp(vector.expected, "OK ", 3u) == 0) {
            char host[300];
            unsigned int port = 0u;
            CHECK_VECTOR(sscanf(vector.expected, "OK %299s %u", host, &port) == 2, vector.name);
            CHECK_VECTOR(status == MAELYS_EGRESS_CHANNEL_OK, vector.name);
            CHECK_VECTOR(reason == NULL, vector.name);
            CHECK_VECTOR(strcmp(request.host, host) == 0, vector.name);
            CHECK_VECTOR(request.host_length == strlen(host), vector.name);
            CHECK_VECTOR(request.port == port, vector.name);
            CHECK_VECTOR(request.version == MAELYS_EGRESS_CHANNEL_PROTOCOL_VERSION, vector.name);
            CHECK_VECTOR(request.protocol == MAELYS_EGRESS_CHANNEL_PROTOCOL_TCP, vector.name);
            unsigned char again[MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE];
            size_t written = maelys_egress_channel_encode_request(
                host, (uint16_t)port, again, sizeof(again));
            CHECK_VECTOR(written == vector.length, vector.name);
            CHECK_VECTOR(memcmp(again, vector.bytes, written) == 0, vector.name);
        } else if (strcmp(vector.expected, "MALFORMED") == 0) {
            CHECK_VECTOR(status == MAELYS_EGRESS_CHANNEL_MALFORMED, vector.name);
            CHECK_VECTOR(reason != NULL && reason[0] != '\0', vector.name);
        } else if (strcmp(vector.expected, "UNSUPPORTED") == 0) {
            CHECK_VECTOR(status == MAELYS_EGRESS_CHANNEL_UNSUPPORTED, vector.name);
            CHECK_VECTOR(reason != NULL && reason[0] != '\0', vector.name);
        } else {
            CHECK_VECTOR(0 && "unknown expectation", vector.name);
        }
    }
    (void)fclose(stream);
    return count;
}

static int replay_responses(void) {
    FILE *stream = open_vectors("responses.txt");
    vector_t vector;
    int count = 0;
    while (read_vector(stream, &vector)) {
        ++count;
        maelys_egress_channel_response_t response;
        const char *reason = (const char *)0x1;
        int valid = maelys_egress_channel_decode_response(
            vector.bytes, vector.length, &response, &reason);
        if (strncmp(vector.expected, "OK ", 3u) == 0) {
            unsigned int status = 0u;
            unsigned int version = 0u;
            CHECK_VECTOR(sscanf(vector.expected, "OK %u %u", &status, &version) == 2, vector.name);
            CHECK_VECTOR(valid == 1, vector.name);
            CHECK_VECTOR(reason == NULL, vector.name);
            CHECK_VECTOR(response.status == status, vector.name);
            CHECK_VECTOR(response.version == version, vector.name);
            if (version == MAELYS_EGRESS_CHANNEL_PROTOCOL_VERSION && response.reserved == 0u &&
                status <= MAELYS_EGRESS_CHANNEL_INTERNAL) {
                unsigned char again[MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE];
                CHECK_VECTOR(maelys_egress_channel_encode_response(
                    (maelys_egress_channel_status_t)status, again, sizeof(again)) ==
                    MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE, vector.name);
                CHECK_VECTOR(memcmp(again, vector.bytes, sizeof(again)) == 0, vector.name);
            }
        } else if (strcmp(vector.expected, "MALFORMED") == 0) {
            CHECK_VECTOR(valid == 0, vector.name);
            CHECK_VECTOR(reason != NULL && reason[0] != '\0', vector.name);
        } else {
            CHECK_VECTOR(0 && "unknown expectation", vector.name);
        }
    }
    (void)fclose(stream);
    return count;
}

static void test_encoder_refusals(void) {
    unsigned char out[MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE];
    char long_host[MAELYS_EGRESS_CHANNEL_MAX_HOST + 2u];
    memset(long_host, 'a', sizeof(long_host) - 1u);
    long_host[sizeof(long_host) - 1u] = '\0';
    CHECK(maelys_egress_channel_encode_request(NULL, 443u, out, sizeof(out)) == 0u);
    CHECK(maelys_egress_channel_encode_request("example.com", 443u, NULL, sizeof(out)) == 0u);
    CHECK(maelys_egress_channel_encode_request("", 443u, out, sizeof(out)) == 0u);
    CHECK(maelys_egress_channel_encode_request("example.com", 0u, out, sizeof(out)) == 0u);
    CHECK(maelys_egress_channel_encode_request(long_host, 443u, out, sizeof(out)) == 0u);
    long_host[MAELYS_EGRESS_CHANNEL_MAX_HOST] = '\0';
    CHECK(maelys_egress_channel_encode_request(long_host, 443u, out, sizeof(out)) ==
          MAELYS_EGRESS_CHANNEL_REQUEST_MAX_SIZE);
    CHECK(maelys_egress_channel_encode_request("example com", 443u, out, sizeof(out)) == 0u);
    CHECK(maelys_egress_channel_encode_request("exa\x7fmple", 443u, out, sizeof(out)) == 0u);
    CHECK(maelys_egress_channel_encode_request("example.com", 443u, out, 20u) == 0u);
    CHECK(maelys_egress_channel_encode_request("example.com", 443u, out, 21u) == 21u);
    unsigned char response[MAELYS_EGRESS_CHANNEL_RESPONSE_SIZE];
    CHECK(maelys_egress_channel_encode_response(MAELYS_EGRESS_CHANNEL_OK, response, 7u) == 0u);
    CHECK(maelys_egress_channel_encode_response(MAELYS_EGRESS_CHANNEL_OK, NULL, 8u) == 0u);
    CHECK(maelys_egress_channel_encode_response(
        (maelys_egress_channel_status_t)256, response, 8u) == 0u);
    CHECK(maelys_egress_channel_encode_response(
        (maelys_egress_channel_status_t)255, response, 8u) == 8u && response[5] == 255u);
    maelys_egress_channel_request_t request;
    const char *reason = NULL;
    CHECK(maelys_egress_channel_decode_request(NULL, 0u, &request, &reason) ==
          MAELYS_EGRESS_CHANNEL_MALFORMED && reason != NULL);
    CHECK(maelys_egress_channel_decode_request(response, 8u, NULL, NULL) ==
          MAELYS_EGRESS_CHANNEL_MALFORMED);
    maelys_egress_channel_response_t decoded;
    CHECK(maelys_egress_channel_decode_response(NULL, 8u, &decoded, NULL) == 0);
    CHECK(maelys_egress_channel_decode_response(response, 8u, NULL, NULL) == 0);
    static const maelys_egress_channel_status_t all[] = {
        MAELYS_EGRESS_CHANNEL_OK, MAELYS_EGRESS_CHANNEL_DENIED, MAELYS_EGRESS_CHANNEL_TIMEOUT,
        MAELYS_EGRESS_CHANNEL_CANCELLED, MAELYS_EGRESS_CHANNEL_MALFORMED,
        MAELYS_EGRESS_CHANNEL_UNSUPPORTED, MAELYS_EGRESS_CHANNEL_RESOURCE,
        MAELYS_EGRESS_CHANNEL_INTERNAL
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        CHECK(strcmp(maelys_egress_channel_status_string(all[i]), "unknown") != 0);
    }
    CHECK(strcmp(maelys_egress_channel_status_string(
        (maelys_egress_channel_status_t)99), "unknown") == 0);
}

int main(void) {
    int requests = replay_requests();
    int responses = replay_responses();
    CHECK(requests >= 20 && responses >= 12);
    test_encoder_refusals();
    (void)printf("channel: %d request vectors, %d response vectors, encoder refusals ok\n",
                 requests, responses);
    return 0;
}
