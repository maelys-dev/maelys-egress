/* Audit observations, not a security pass/fail test. No network access. */
#include "src/internal.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static void http_case(const char *name, const char *wire) {
    maelys_egress_config_t config = {.unauthenticated_loopback = 1};
    egress_proxy_request_t request = {0};
    char *error = NULL;
    int result = egress_parse_http_request((const unsigned char *)wire,
        strlen(wire), &config, &request, &error);
    printf("%s: result=%d; output=", name, result);
    for (size_t i = 0; i < request.forward_length; ++i) {
        unsigned char c = request.forward_bytes[i];
        if (c < 0x20 || c == 0x7f) printf("\\x%02x", c);
        else putchar(c);
    }
    printf("; error=%s\n", error ? error : "none");
    egress_proxy_request_clear(&request);
    maelys_egress_error_free(error);
}

/* The sole caller supplies a short, fixed name and a handshake-sized buffer. */
static size_t hello(const unsigned char *name, size_t length, unsigned char *b) {
    size_t extensions = 9 + length, body = 43 + extensions;
    size_t handshake = 4 + body, n = 0;
    b[n++] = 22; b[n++] = 3; b[n++] = 3;
    b[n++] = (unsigned char)(handshake >> 8); b[n++] = (unsigned char)handshake;
    b[n++] = 1; b[n++] = (unsigned char)(body >> 16);
    b[n++] = (unsigned char)(body >> 8); b[n++] = (unsigned char)body;
    b[n++] = 3; b[n++] = 3;
    memset(b + n, 0x5a, 32); n += 32;
    b[n++] = 0;
    b[n++] = 0; b[n++] = 2; b[n++] = 0x13; b[n++] = 1;
    b[n++] = 1; b[n++] = 0;
    b[n++] = (unsigned char)(extensions >> 8); b[n++] = (unsigned char)extensions;
    b[n++] = 0; b[n++] = 0;
    b[n++] = (unsigned char)((length + 5) >> 8); b[n++] = (unsigned char)(length + 5);
    b[n++] = (unsigned char)((length + 3) >> 8); b[n++] = (unsigned char)(length + 3);
    b[n++] = 0;
    b[n++] = (unsigned char)(length >> 8); b[n++] = (unsigned char)length;
    memcpy(b + n, name, length);
    return n + length;
}

int main(void) {
    http_case("valid", "GET http://example.com/path HTTP/1.1\r\nHost: example.com\r\n\r\n");
    http_case("bare CR", "GET http://example.com/path HTTP/1.1\r\nHost: example.com\r\nX: a\rb\r\n\r\n");
    http_case("tab target", "GET http://example.com/a\tb HTTP/1.1\r\nHost: example.com\r\n\r\n");
    http_case("DEL header", "GET http://example.com/path HTTP/1.1\r\nHost: example.com\r\nX: a\177b\r\n\r\n");
    http_case("query-only URI", "GET http://example.com?x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    unsigned char bytes[EGRESS_HANDSHAKE_MAX];
    static const unsigned char sni[] = "example.com\0.other.example";
    size_t length = hello(sni, sizeof(sni) - 1, bytes);
    char *error = NULL;
    int result = egress_tls_client_hello_matches(bytes, length, "example.com", &error);
    printf("embedded-NUL SNI: result=%d, error=%s\n", result, error ? error : "none");
    maelys_egress_error_free(error);
    static const char *addresses[] = {
        "64:ff9b:1::1", "100:0:0:1::1", "2001:1::1", "2001:3::1", "2001:4:112::1"
    };
    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        struct sockaddr_in6 addr = {.sin6_family = AF_INET6};
        if (inet_pton(AF_INET6, addresses[i], &addr.sin6_addr) != 1) return 1;
        printf("IPv6 %s: blocked=%d\n", addresses[i],
            egress_address_is_private((const struct sockaddr *)&addr));
    }
    return 0;
}
