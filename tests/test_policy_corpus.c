/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Plays maelys-sandbox-policy's network destination corpus
 * (tests/vectors/policy-destinations, copied from a tag of that repository)
 * against what Egress enforces. The policy is built through the public
 * calls, sealed through the real sealing code with the resolver substituted,
 * and each request is decided by the functions the server uses: the host
 * canonicalisation, the sealed lookup, and the ClientHello identity guard.
 * No network is used: the corpus states what each name resolves to.
 *
 *   test-policy-corpus CORPUS_DIRECTORY
 *
 * Egress resolves exact names when it seals a policy. A request marked
 * private is therefore played against a seal in which its name resolves to
 * a private address, and the contract admits the refusal of the whole policy
 * before launch in that case; such results are counted apart. A case Egress
 * refuses when the policy is built is conformant only if the corpus refuses
 * it at the source or needs a capability Egress does not announce.
 */
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* The resolver the sealing code calls is this file's. */
static const char *corpus_private_name;
#define getaddrinfo corpus_getaddrinfo
#define freeaddrinfo corpus_freeaddrinfo
#define gai_strerror corpus_gai_strerror
static int corpus_getaddrinfo(const char *node, const char *service,
    const struct addrinfo *hints, struct addrinfo **out);
static void corpus_freeaddrinfo(struct addrinfo *list);
static const char *corpus_gai_strerror(int code);
#include "src/core/policy.c"
#undef getaddrinfo
#undef freeaddrinfo
#undef gai_strerror

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A strict literal resolves to itself; the name a request marks private to
 * 10.0.0.1; every other name to 93.184.216.34, a global address. */
static int corpus_getaddrinfo(const char *node, const char *service,
    const struct addrinfo *hints, struct addrinfo **out) {
    (void)hints;
    struct addrinfo *entry = calloc(1u, sizeof(*entry));
    struct sockaddr_storage *address = calloc(1u, sizeof(*address));
    if (!entry || !address) { free(entry); free(address); return EAI_MEMORY; }
    uint16_t port = (uint16_t)strtoul(service, NULL, 10);
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)address;
    struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)address;
    if (inet_pton(AF_INET6, node, &ipv6->sin6_addr) == 1) {
        ipv6->sin6_family = AF_INET6; ipv6->sin6_port = htons(port);
        entry->ai_addrlen = sizeof(*ipv6);
    } else {
        const char *chosen = inet_pton(AF_INET, node, &ipv4->sin_addr) == 1 ? node :
            corpus_private_name && strcmp(node, corpus_private_name) == 0 ?
            "10.0.0.1" : "93.184.216.34";
        (void)inet_pton(AF_INET, chosen, &ipv4->sin_addr);
        ipv4->sin_family = AF_INET; ipv4->sin_port = htons(port);
        entry->ai_addrlen = sizeof(*ipv4);
    }
    entry->ai_family = ((struct sockaddr *)address)->sa_family;
    entry->ai_socktype = SOCK_STREAM;
    entry->ai_protocol = IPPROTO_TCP;
    entry->ai_addr = (struct sockaddr *)address;
    *out = entry;
    return 0;
}

static void corpus_freeaddrinfo(struct addrinfo *list) {
    while (list) {
        struct addrinfo *next = list->ai_next;
        free(list->ai_addr);
        free(list);
        list = next;
    }
}

static const char *corpus_gai_strerror(int code) {
    (void)code;
    return "corpus resolver failure";
}

/* A ClientHello naming host, or carrying no server_name when host is NULL. */
static size_t client_hello(const char *host, unsigned char output[EGRESS_HANDSHAKE_MAX]) {
    size_t host_length = host ? strlen(host) : 0u;
    size_t extension_length = host ? 9u + host_length : 0u;
    size_t body_length = 43u + extension_length;
    size_t handshake_length = 4u + body_length;
    size_t used = 0u;
    output[used++] = 22u; output[used++] = 3u; output[used++] = 3u;
    output[used++] = (unsigned char)(handshake_length >> 8u);
    output[used++] = (unsigned char)handshake_length;
    output[used++] = 1u;
    output[used++] = (unsigned char)(body_length >> 16u);
    output[used++] = (unsigned char)(body_length >> 8u);
    output[used++] = (unsigned char)body_length;
    output[used++] = 3u; output[used++] = 3u;
    memset(output + used, 0x5au, 32u); used += 32u;
    output[used++] = 0u;
    output[used++] = 0u; output[used++] = 2u;
    output[used++] = 0x13u; output[used++] = 0x01u;
    output[used++] = 1u; output[used++] = 0u;
    output[used++] = (unsigned char)(extension_length >> 8u);
    output[used++] = (unsigned char)extension_length;
    if (host) {
        output[used++] = 0u; output[used++] = 0u;
        size_t sni_length = 5u + host_length;
        output[used++] = (unsigned char)(sni_length >> 8u);
        output[used++] = (unsigned char)sni_length;
        size_t list_length = 3u + host_length;
        output[used++] = (unsigned char)(list_length >> 8u);
        output[used++] = (unsigned char)list_length;
        output[used++] = 0u;
        output[used++] = (unsigned char)(host_length >> 8u);
        output[used++] = (unsigned char)host_length;
        memcpy(output + used, host, host_length); used += host_length;
    }
    return used;
}

#define MAX_ITEMS 32
#define NAME_BYTES 260

typedef struct corpus_destination {
    char name[NAME_BYTES];
    unsigned int port;
    int require_sni, allow_private;
} corpus_destination_t;

typedef struct corpus_request {
    char name[NAME_BYTES], sni[NAME_BYTES], reason[40];
    unsigned int port;
    int has_sni, is_private, allowed;
} corpus_request_t;

typedef struct corpus_case {
    char mode[16];
    corpus_destination_t destinations[MAX_ITEMS];
    size_t destination_count;
    corpus_request_t requests[MAX_ITEMS];
    size_t request_count;
    int requires_wildcard, compile_refused, compile_seen;
} corpus_case_t;

static int failures;
static unsigned long enforced, before_launch, at_source, not_mediated;

#define NONCONFORMANT(file, ...) do { \
    fprintf(stderr, "NONCONFORMANT %s: ", file); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); ++failures; } while (0)

/* Every word of every directive is read; anything left over or unknown is a
 * defect of the copy, never something to skip. */
static int parse_case(const char *path, corpus_case_t *c) {
    memset(c, 0, sizeof(*c));
    strcpy(c->mode, "mediated");
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    char line[1024];
    int ok = 1;
    while (ok && fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *words[10];
        size_t n = 0u;
        for (char *w = strtok(line, " \t"); w && n < 10u; w = strtok(NULL, " \t")) words[n++] = w;
        if (n == 0u) continue;
        if (!strcmp(words[0], "network") && n == 2u) {
            snprintf(c->mode, sizeof(c->mode), "%s", words[1]);
        } else if (!strcmp(words[0], "destination") && n >= 4u && !strcmp(words[1], "tcp") &&
                   c->destination_count < MAX_ITEMS && strlen(words[2]) < NAME_BYTES) {
            corpus_destination_t *d = &c->destinations[c->destination_count++];
            strcpy(d->name, words[2]);
            d->port = (unsigned int)strtoul(words[3], NULL, 10);
            for (size_t k = 4u; k < n; ++k) {
                if (!strcmp(words[k], "require-tls-sni")) d->require_sni = 1;
                else if (!strcmp(words[k], "allow-private-addresses")) d->allow_private = 1;
                else ok = 0;
            }
        } else if (!strcmp(words[0], "requires") && n == 2u &&
                   !strcmp(words[1], "network-host-wildcard")) {
            c->requires_wildcard = 1;
        } else if (!strcmp(words[0], "request") && n >= 6u && !strcmp(words[1], "tcp") &&
                   c->request_count < MAX_ITEMS && strlen(words[2]) < NAME_BYTES &&
                   !strncmp(words[4], "sni=", 4u) && strlen(words[4]) - 4u < NAME_BYTES) {
            corpus_request_t *r = &c->requests[c->request_count++];
            strcpy(r->name, words[2]);
            r->port = (unsigned int)strtoul(words[3], NULL, 10);
            if (strcmp(words[4] + 4, "absent")) { r->has_sni = 1; strcpy(r->sni, words[4] + 4); }
            size_t k = 5u;
            if (k < n && !strcmp(words[k], "private")) { r->is_private = 1; ++k; }
            if (k + 1u == n && !strcmp(words[k], "allowed")) r->allowed = 1;
            else if (k + 2u == n && !strcmp(words[k], "refused") && strlen(words[k + 1u]) < 40u)
                strcpy(r->reason, words[k + 1u]);
            else ok = 0;
        } else if (!strcmp(words[0], "compile") && n == 2u &&
                   (!strcmp(words[1], "accepted") || !strcmp(words[1], "refused"))) {
            c->compile_seen = 1;
            c->compile_refused = !strcmp(words[1], "refused");
        } else {
            ok = 0;
        }
    }
    fclose(file);
    return ok && c->compile_seen;
}

/* The policy as Egress builds it. NULL when a call refuses it. */
static maelys_egress_policy_t *build(const corpus_case_t *c) {
    maelys_egress_policy_t *policy = NULL;
    char *error = NULL;
    if (maelys_egress_policy_create(&policy, &error) != MAELYS_EGRESS_OK) return NULL;
    for (size_t i = 0u; i < c->destination_count; ++i) {
        const corpus_destination_t *d = &c->destinations[i];
        if (d->port == 0u || d->port > 65535u ||
            maelys_egress_policy_allow_tcp(policy, d->name, (uint16_t)d->port,
                d->allow_private, &error) != MAELYS_EGRESS_OK ||
            (d->require_sni && maelys_egress_policy_require_tls_sni(policy, d->name,
                (uint16_t)d->port, &error) != MAELYS_EGRESS_OK)) {
            maelys_egress_error_free(error);
            maelys_egress_policy_destroy(policy);
            return NULL;
        }
    }
    return policy;
}

/* What Egress does with the request against a sealed policy: allowed, or
 * the contract's identifier of its refusal. */
static const char *decide(const maelys_egress_policy_t *policy, const corpus_request_t *r) {
    char canonical[EGRESS_MAX_HOST + 1u];
    if (!egress_canonical_host(r->name, canonical) || strcmp(canonical, r->name) != 0)
        return "malformed-name";
    const egress_destination_t *d = r->port && r->port <= 65535u ?
        egress_policy_find(policy, canonical, (uint16_t)r->port) : NULL;
    if (!d) return "no-destination";
    if (d->require_tls_sni) {
        unsigned char hello[EGRESS_HANDSHAKE_MAX];
        size_t length = client_hello(r->has_sni ? r->sni : NULL, hello);
        char *error = NULL;
        int match = egress_tls_client_hello_matches(hello, length, d->host, &error);
        maelys_egress_error_free(error);
        if (match != 1) return r->has_sni ? "sni-mismatch" : "sni-absent";
    }
    return NULL;
}

static void play(const char *file, const corpus_case_t *c) {
    if (!strcmp(c->mode, "direct")) { ++not_mediated; return; }
    if (!strcmp(c->mode, "none")) {
        /* No destination: Egress cannot seal an empty policy, so nothing
         * is ever relayed. Every request is refused before launch. */
        maelys_egress_policy_t *policy = NULL;
        char *error = NULL;
        int sealed = maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK &&
            maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK;
        maelys_egress_error_free(error);
        maelys_egress_policy_destroy(policy);
        if (sealed) NONCONFORMANT(file, "an empty policy was sealed");
        for (size_t i = 0u; i < c->request_count; ++i) {
            if (c->requests[i].allowed) NONCONFORMANT(file, "network none allows a request");
            ++before_launch;
        }
        return;
    }
    maelys_egress_policy_t *policy = build(c);
    if (!policy) {
        if (!c->compile_refused && !c->requires_wildcard)
            NONCONFORMANT(file, "Egress refuses a policy the corpus accepts");
        ++at_source;
        return;
    }
    maelys_egress_policy_destroy(policy);
    if (c->compile_refused) {
        NONCONFORMANT(file, "Egress builds a policy the corpus refuses at the source");
        return;
    }
    if (c->requires_wildcard) {
        NONCONFORMANT(file, "Egress builds a policy that needs network-host-wildcard");
        return;
    }
    for (size_t i = 0u; i < c->request_count; ++i) {
        const corpus_request_t *r = &c->requests[i];
        corpus_private_name = r->is_private ? r->name : NULL;
        policy = build(c);
        char *error = NULL;
        maelys_egress_result_t sealed = policy ?
            maelys_egress_policy_seal(policy, &error) : MAELYS_EGRESS_ERR_ARGUMENT;
        maelys_egress_error_free(error);
        if (sealed == MAELYS_EGRESS_ERR_DENIED && r->is_private) {
            /* The contract admits it, for a request that is not allowed. */
            if (r->allowed) NONCONFORMANT(file, "%s:%u private and allowed, refused before launch",
                                          r->name, r->port);
            ++before_launch;
        } else if (sealed != MAELYS_EGRESS_OK) {
            NONCONFORMANT(file, "the policy did not seal for %s:%u", r->name, r->port);
        } else {
            const char *refusal = decide(policy, r);
            if (r->allowed && refusal) {
                NONCONFORMANT(file, "%s:%u expected allowed, refused %s", r->name, r->port, refusal);
            } else if (!r->allowed && !refusal) {
                NONCONFORMANT(file, "%s:%u expected refused %s, allowed", r->name, r->port, r->reason);
            } else if (!r->allowed && strcmp(refusal, r->reason) != 0) {
                NONCONFORMANT(file, "%s:%u expected refused %s, refused %s",
                              r->name, r->port, r->reason, refusal);
            }
            ++enforced;
        }
        maelys_egress_policy_destroy(policy);
        corpus_private_name = NULL;
    }
}

static int by_name(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s CORPUS_DIRECTORY\n", argv[0]); return 2; }
    char cases_path[1024];
    snprintf(cases_path, sizeof(cases_path), "%s/cases", argv[1]);
    DIR *directory = opendir(cases_path);
    if (!directory) { perror(cases_path); return 2; }
    char *names[128];
    size_t count = 0u;
    for (struct dirent *entry; (entry = readdir(directory)) && count < 128u;) {
        size_t length = strlen(entry->d_name);
        if (length > 5u && !strcmp(entry->d_name + length - 5u, ".case"))
            names[count++] = strdup(entry->d_name);
    }
    closedir(directory);
    qsort(names, count, sizeof(names[0]), by_name);
    for (size_t i = 0u; i < count; ++i) {
        char path[1400];
        snprintf(path, sizeof(path), "%s/%s", cases_path, names[i]);
        corpus_case_t c;
        if (!parse_case(path, &c)) NONCONFORMANT(names[i], "the case does not parse");
        else play(names[i], &c);
        free(names[i]);
    }
    if (count == 0u) NONCONFORMANT(cases_path, "no case");
    printf("policy destination corpus: %zu cases; %lu requests enforced, %lu refused before "
           "launch, %lu policies refused at the source, %lu not mediated (direct)%s\n",
           count, enforced, before_launch, at_source, not_mediated,
           failures ? "" : "; conformant");
    return failures ? 1 : 0;
}
