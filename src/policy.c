#include "src/internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static egress_destination_t *find_mutable_destination(
    maelys_egress_policy_t *policy, const char *host, uint16_t port) {
    if (!policy || policy->sealed) return NULL;
    for (size_t i = 0; i < policy->count; ++i) {
        if (policy->destinations[i].port == port &&
            strcmp(policy->destinations[i].host, host) == 0) {
            return &policy->destinations[i];
        }
    }
    return NULL;
}

maelys_egress_result_t maelys_egress_policy_create(
    maelys_egress_policy_t **out_policy,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!out_policy) return MAELYS_EGRESS_ERR_ARGUMENT;
    *out_policy = calloc(1, sizeof(**out_policy));
    if (!*out_policy) return MAELYS_EGRESS_ERR_MEMORY;
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_policy_require_tls_sni(
    maelys_egress_policy_t *policy,
    const char *canonical_host,
    uint16_t port,
    char **out_error) {
    if (out_error) *out_error = NULL;
    char host[EGRESS_MAX_HOST + 1u];
    struct in_addr ipv4;
    struct in6_addr ipv6;
    if (!policy || policy->sealed || !canonical_host || port == 0u ||
        !egress_canonical_host(canonical_host, host)) {
        egress_set_error(out_error, "mutable policy, DNS host and non-zero port are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    if (inet_pton(AF_INET, host, &ipv4) == 1 ||
        inet_pton(AF_INET6, host, &ipv6) == 1) {
        egress_set_error(out_error, "TLS SNI identity cannot be required for numeric host %s", host);
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    egress_destination_t *destination = find_mutable_destination(policy, host, port);
    if (!destination) {
        egress_set_error(out_error, "TLS SNI destination %s:%u must be allowed first",
                       host, (unsigned int)port);
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    destination->require_tls_sni = 1;
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_policy_allow_tcp(
    maelys_egress_policy_t *policy,
    const char *canonical_host,
    uint16_t port,
    int allow_private_addresses,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!policy || policy->sealed || !canonical_host || port == 0u) {
        egress_set_error(out_error, "mutable policy, host and non-zero port are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    char host[EGRESS_MAX_HOST + 1u];
    if (!egress_canonical_host(canonical_host, host)) {
        egress_set_error(out_error, "invalid ASCII DNS name or numeric address: %s",
                       canonical_host);
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    for (size_t i = 0; i < policy->count; ++i) {
        if (policy->destinations[i].port == port &&
            strcmp(policy->destinations[i].host, host) == 0) {
            egress_set_error(out_error, "duplicate destination %s:%u", host,
                           (unsigned int)port);
            return MAELYS_EGRESS_ERR_ARGUMENT;
        }
    }
    if (policy->count >= EGRESS_MAX_DESTINATIONS) {
        egress_set_error(out_error, "policy exceeds %u destinations",
                       (unsigned int)EGRESS_MAX_DESTINATIONS);
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    if (policy->count == policy->capacity) {
        size_t next = policy->capacity ? policy->capacity * 2u : 8u;
        egress_destination_t *grown = realloc(
            policy->destinations, next * sizeof(*grown));
        if (!grown) return MAELYS_EGRESS_ERR_MEMORY;
        policy->destinations = grown;
        policy->capacity = next;
    }
    char *copy = egress_strdup(host);
    if (!copy) return MAELYS_EGRESS_ERR_MEMORY;
    policy->destinations[policy->count++] = (egress_destination_t){
        .host = copy,
        .port = port,
        .allow_private = allow_private_addresses != 0
    };
    return MAELYS_EGRESS_OK;
}

static int address_equal(const egress_address_t *left, const egress_address_t *right) {
    return left->length == right->length &&
        memcmp(&left->storage, &right->storage, left->length) == 0;
}

static int address_compare(const void *left, const void *right) {
    const egress_address_t *a = left;
    const egress_address_t *b = right;
    int family_a = ((const struct sockaddr *)&a->storage)->sa_family;
    int family_b = ((const struct sockaddr *)&b->storage)->sa_family;
    if (family_a != family_b) return family_a < family_b ? -1 : 1;
    if (family_a == AF_INET) {
        const struct sockaddr_in *ipv4_a = (const struct sockaddr_in *)&a->storage;
        const struct sockaddr_in *ipv4_b = (const struct sockaddr_in *)&b->storage;
        return memcmp(&ipv4_a->sin_addr, &ipv4_b->sin_addr,
                      sizeof(ipv4_a->sin_addr));
    }
    const struct sockaddr_in6 *ipv6_a = (const struct sockaddr_in6 *)&a->storage;
    const struct sockaddr_in6 *ipv6_b = (const struct sockaddr_in6 *)&b->storage;
    return memcmp(&ipv6_a->sin6_addr, &ipv6_b->sin6_addr,
                  sizeof(ipv6_a->sin6_addr));
}

static maelys_egress_result_t resolve_destination(
    egress_destination_t *destination,
    char **out_error) {
    char service[6];
    (void)snprintf(service, sizeof(service), "%u", (unsigned int)destination->port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *addresses = NULL;
    int lookup = getaddrinfo(destination->host, service, &hints, &addresses);
    if (lookup != 0) {
        egress_set_error(out_error, "cannot resolve pinned destination %s:%u: %s",
                       destination->host, (unsigned int)destination->port,
                       gai_strerror(lookup));
        return MAELYS_EGRESS_ERR_IO;
    }
    maelys_egress_result_t result = MAELYS_EGRESS_OK;
    for (const struct addrinfo *address = addresses;
         address; address = address->ai_next) {
        if (address->ai_addrlen > sizeof(struct sockaddr_storage) ||
            (address->ai_family != AF_INET && address->ai_family != AF_INET6)) {
            continue;
        }
        if (!destination->allow_private && egress_address_is_private(address->ai_addr)) {
            egress_set_error(out_error,
                "destination %s:%u resolves to a non-global address; opt in explicitly",
                destination->host, (unsigned int)destination->port);
            result = MAELYS_EGRESS_ERR_DENIED;
            break;
        }
        egress_address_t candidate = {.length = (socklen_t)address->ai_addrlen};
        memcpy(&candidate.storage, address->ai_addr, address->ai_addrlen);
        int duplicate = 0;
        for (size_t i = 0; i < destination->address_count; ++i) {
            if (address_equal(&candidate, &destination->addresses[i])) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) continue;
        if (destination->address_count >= EGRESS_MAX_PINNED_ADDRESSES) {
            egress_set_error(out_error, "destination %s:%u resolves to too many addresses",
                           destination->host, (unsigned int)destination->port);
            result = MAELYS_EGRESS_ERR_ARGUMENT;
            break;
        }
        destination->addresses[destination->address_count++] = candidate;
    }
    freeaddrinfo(addresses);
    if (result == MAELYS_EGRESS_OK) {
        qsort(destination->addresses, destination->address_count,
              sizeof(destination->addresses[0]), address_compare);
    }
    if (result == MAELYS_EGRESS_OK && destination->address_count == 0u) {
        egress_set_error(out_error, "destination %s:%u has no usable TCP address",
                       destination->host, (unsigned int)destination->port);
        result = MAELYS_EGRESS_ERR_IO;
    }
    return result;
}

static int destination_compare(const void *left, const void *right) {
    const egress_destination_t *a = left;
    const egress_destination_t *b = right;
    int host = strcmp(a->host, b->host);
    if (host != 0) return host;
    return a->port < b->port ? -1 : a->port > b->port ? 1 : 0;
}

void egress_policy_compute_digest(maelys_egress_policy_t *policy) {
    size_t capacity = 16u;
    for (size_t i = 0; i < policy->count; ++i) {
        capacity += strlen(policy->destinations[i].host) + 32u +
            policy->destinations[i].address_count * (INET6_ADDRSTRLEN + 16u);
    }
    char *canonical = malloc(capacity);
    if (!canonical) {
        policy->digest_hex[0] = '\0';
        return;
    }
    size_t used = 0u;
    for (size_t i = 0; i < policy->count; ++i) {
        int written = snprintf(canonical + used, capacity - used, "tcp\t%s\t%u\t%d\t%d\n",
            policy->destinations[i].host,
            (unsigned int)policy->destinations[i].port,
            policy->destinations[i].allow_private,
            policy->destinations[i].require_tls_sni);
        if (written < 0 || (size_t)written >= capacity - used) {
            free(canonical);
            policy->digest_hex[0] = '\0';
            return;
        }
        used += (size_t)written;
        for (size_t j = 0; j < policy->destinations[i].address_count; ++j) {
            const egress_address_t *address = &policy->destinations[i].addresses[j];
            const void *raw = NULL;
            int family = ((const struct sockaddr *)&address->storage)->sa_family;
            if (family == AF_INET) {
                raw = &((const struct sockaddr_in *)&address->storage)->sin_addr;
            } else if (family == AF_INET6) {
                raw = &((const struct sockaddr_in6 *)&address->storage)->sin6_addr;
            } else {
                free(canonical);
                policy->digest_hex[0] = '\0';
                return;
            }
            char numeric[INET6_ADDRSTRLEN];
            if (!inet_ntop(family, raw, numeric, sizeof(numeric))) {
                free(canonical);
                policy->digest_hex[0] = '\0';
                return;
            }
            written = snprintf(canonical + used, capacity - used, "addr\t%d\t%s\n",
                               family == AF_INET ? 4 : 6, numeric);
            if (written < 0 || (size_t)written >= capacity - used) {
                free(canonical);
                policy->digest_hex[0] = '\0';
                return;
            }
            used += (size_t)written;
        }
    }
    egress_sha256_hex(canonical, used, policy->digest_hex);
    free(canonical);
}

maelys_egress_result_t maelys_egress_policy_seal(
    maelys_egress_policy_t *policy,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!policy || policy->sealed || policy->count == 0u) {
        egress_set_error(out_error, "a non-empty mutable policy is required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    qsort(policy->destinations, policy->count,
          sizeof(*policy->destinations), destination_compare);
    for (size_t i = 0; i < policy->count; ++i) {
        maelys_egress_result_t result = resolve_destination(
            &policy->destinations[i], out_error);
        if (result != MAELYS_EGRESS_OK) return result;
    }
    egress_policy_compute_digest(policy);
    if (!policy->digest_hex[0]) return MAELYS_EGRESS_ERR_MEMORY;
    policy->sealed = 1;
    return MAELYS_EGRESS_OK;
}

maelys_egress_result_t maelys_egress_policy_reseal(
    const maelys_egress_policy_t *source,
    maelys_egress_policy_t **out_policy,
    char **out_error) {
    if (out_error) *out_error = NULL;
    if (!source || !source->sealed || !out_policy) {
        egress_set_error(out_error, "sealed source and output policy are required");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    *out_policy = NULL;
    maelys_egress_policy_t *copy = NULL;
    maelys_egress_result_t result = maelys_egress_policy_create(&copy, out_error);
    for (size_t i = 0; result == MAELYS_EGRESS_OK && i < source->count; ++i) {
        const egress_destination_t *destination = &source->destinations[i];
        result = maelys_egress_policy_allow_tcp(copy, destination->host,
            destination->port, destination->allow_private, out_error);
        if (result == MAELYS_EGRESS_OK && destination->require_tls_sni) {
            result = maelys_egress_policy_require_tls_sni(copy, destination->host,
                destination->port, out_error);
        }
    }
    if (result == MAELYS_EGRESS_OK) result = maelys_egress_policy_seal(copy, out_error);
    if (result != MAELYS_EGRESS_OK) {
        maelys_egress_policy_destroy(copy);
        return result;
    }
    *out_policy = copy;
    return MAELYS_EGRESS_OK;
}

int maelys_egress_policy_is_sealed(const maelys_egress_policy_t *policy) {
    return policy && policy->sealed;
}

const char *maelys_egress_policy_digest_hex(const maelys_egress_policy_t *policy) {
    return policy && policy->sealed ? policy->digest_hex : NULL;
}

const egress_destination_t *egress_policy_find(
    const maelys_egress_policy_t *policy,
    const char *host,
    uint16_t port) {
    if (!policy || !policy->sealed || !host || port == 0u) return NULL;
    for (size_t i = 0; i < policy->count; ++i) {
        const egress_destination_t *destination = &policy->destinations[i];
        int comparison = strcmp(host, destination->host);
        if (comparison == 0 && port == destination->port) return destination;
        if (comparison < 0) break;
    }
    return NULL;
}

void maelys_egress_policy_destroy(maelys_egress_policy_t *policy) {
    if (!policy) return;
    for (size_t i = 0; i < policy->count; ++i) free(policy->destinations[i].host);
    free(policy->destinations);
    free(policy);
}
