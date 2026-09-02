#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "src/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static void hex_encode(const unsigned char *bytes, size_t length, char *output) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        output[i * 2u] = hex[bytes[i] >> 4u];
        output[i * 2u + 1u] = hex[bytes[i] & 0x0fu];
    }
    output[length * 2u] = '\0';
}

int egress_hmac_sha256(
    const unsigned char *key, size_t key_length,
    const void *data, size_t data_length,
    unsigned char output[32]) {
    unsigned char normalized[64] = {0};
    if (key_length > sizeof(normalized)) {
        egress_sha256(key, key_length, normalized);
    } else {
        memcpy(normalized, key, key_length);
    }
    unsigned char inner_pad[64];
    unsigned char outer_pad[64];
    for (size_t i = 0; i < sizeof(normalized); ++i) {
        inner_pad[i] = (unsigned char)(normalized[i] ^ 0x36u);
        outer_pad[i] = (unsigned char)(normalized[i] ^ 0x5cu);
    }
    if (data_length > SIZE_MAX - sizeof(inner_pad)) {
        memset(output, 0, 32u);
        return 0;
    }
    unsigned char *inner = malloc(sizeof(inner_pad) + data_length);
    if (!inner) {
        memset(output, 0, 32u);
        return 0;
    }
    memcpy(inner, inner_pad, sizeof(inner_pad));
    memcpy(inner + sizeof(inner_pad), data, data_length);
    unsigned char inner_digest[32];
    egress_sha256(inner, sizeof(inner_pad) + data_length, inner_digest);
    free(inner);
    unsigned char outer[sizeof(outer_pad) + sizeof(inner_digest)];
    memcpy(outer, outer_pad, sizeof(outer_pad));
    memcpy(outer + sizeof(outer_pad), inner_digest, sizeof(inner_digest));
    egress_sha256(outer, sizeof(outer), output);
    egress_secure_zero(normalized, sizeof(normalized));
    egress_secure_zero(inner_digest, sizeof(inner_digest));
    return 1;
}

static int quoted_field(
    const char *line, const char *marker, char *output, size_t capacity) {
    const char *begin = strstr(line, marker);
    if (!begin) return 0;
    begin += strlen(marker);
    const char *end = strchr(begin, '"');
    size_t length = end ? (size_t)(end - begin) : SIZE_MAX;
    if (!end || length >= capacity || memchr(begin, '\\', length)) return 0;
    memcpy(output, begin, length);
    output[length] = '\0';
    return 1;
}

static int resume_audit(maelys_egress_audit_t *audit, char **out_error) {
    if (lseek(audit->fd, 0, SEEK_SET) < 0) return 0;
    char line[4096];
    size_t used = 0u;
    uint64_t sequence = 0u;
    for (;;) {
        char byte;
        ssize_t amount = read(audit->fd, &byte, 1u);
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) return 0;
        if (amount == 0 && used == 0u) break;
        if (amount == 0 || used + 1u >= sizeof(line)) {
            errno = EINVAL;
            egress_set_error(out_error, "audit log ends with a partial or oversized record");
            return 0;
        }
        line[used++] = byte;
        if (byte != '\n') continue;
        line[used] = '\0';
        const char *sequence_marker = strstr(line, "{\"v\":1,\"seq\":");
        char key_id[64], previous[65], mac_hex[65], canonical[2048];
        char *number_end = NULL;
        unsigned long long parsed_sequence = sequence_marker ?
            strtoull(sequence_marker + 13u, &number_end, 10) : 0u;
        int valid = sequence_marker && number_end && *number_end == ',' &&
            parsed_sequence == (unsigned long long)(sequence + 1u) &&
            quoted_field(line, "\"key_id\":\"", key_id, sizeof(key_id)) &&
            quoted_field(line, "\"previous\":\"", previous, sizeof(previous)) &&
            quoted_field(line, "\"mac\":\"", mac_hex, sizeof(mac_hex)) &&
            quoted_field(line, "\"canonical\":\"", canonical, sizeof(canonical)) &&
            strcmp(key_id, audit->key_id) == 0 && strlen(previous) == 64u &&
            strlen(mac_hex) == 64u && strcmp(previous, audit->chain_hex) == 0;
        size_t canonical_length = valid ? strlen(canonical) : 0u;
        char expected_suffix[71];
        (void)snprintf(expected_suffix, sizeof(expected_suffix), "|prev=%s", previous);
        size_t suffix_length = strlen(expected_suffix);
        valid = valid && canonical_length >= suffix_length &&
            strcmp(canonical + canonical_length - suffix_length, expected_suffix) == 0;
        unsigned char expected[32];
        char expected_hex[65];
        valid = valid && egress_hmac_sha256(audit->key, audit->key_length,
            canonical, canonical_length, expected);
        if (valid) {
            hex_encode(expected, sizeof(expected), expected_hex);
            valid = egress_constant_time_equal(expected_hex, mac_hex);
        }
        char exact[4096];
        int exact_length = valid ? snprintf(exact, sizeof(exact),
            "{\"v\":1,\"seq\":%llu,\"key_id\":\"%s\",\"previous\":\"%s\","
            "\"mac\":\"%s\",\"canonical\":\"%s\"}\n",
            parsed_sequence, key_id, previous, mac_hex, canonical) : -1;
        valid = valid && exact_length > 0 && (size_t)exact_length < sizeof(exact) &&
            (size_t)exact_length == used && memcmp(exact, line, used) == 0;
        egress_secure_zero(expected, sizeof(expected));
        if (!valid) {
            errno = EINVAL;
            egress_set_error(out_error,
                "audit record %llu fails sequence, key, chain or HMAC verification",
                (unsigned long long)(sequence + 1u));
            return 0;
        }
        ++sequence;
        memcpy(audit->chain_hex, mac_hex, sizeof(audit->chain_hex));
        used = 0u;
    }
    atomic_store(&audit->records, sequence);
    return 1;
}

maelys_egress_result_t maelys_egress_audit_file_create(
    const char *path, const void *key, size_t key_length, const char *key_id,
    maelys_egress_audit_t **out_audit, char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_audit) *out_audit = NULL;
    if (!path || !key || key_length < 16u || key_length > 4096u ||
        !key_id || !key_id[0] || strlen(key_id) >= 64u || !out_audit) {
        egress_set_error(out_error, "audit requires path, 16..4096-byte key and key id");
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    for (const unsigned char *p = (const unsigned char *)key_id; *p; ++p) {
        if (*p < 0x21u || *p > 0x7eu || *p == '"' || *p == '\\') {
            egress_set_error(out_error, "audit key id must be safe visible ASCII");
            return MAELYS_EGRESS_ERR_ARGUMENT;
        }
    }
    int flags = O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags, S_IRUSR | S_IWUSR);
    struct stat status;
    if (fd < 0 || fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_nlink != 1u || status.st_uid != geteuid() ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0u || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved = fd < 0 ? errno : EPERM;
        if (fd >= 0) (void)maelys_sys_fd_close(&fd);
        errno = saved;
        egress_set_error(out_error,
            "audit log must be exclusively held, owner-only, regular and non-symlink: %s",
            strerror(saved));
        return MAELYS_EGRESS_ERR_DENIED;
    }
    maelys_egress_audit_t *audit = calloc(1, sizeof(*audit));
    if (!audit) { (void)maelys_sys_fd_close(&fd); return MAELYS_EGRESS_ERR_MEMORY; }
    audit->key = malloc(key_length);
    if (!audit->key || pthread_mutex_init(&audit->lock, NULL) != 0) {
        free(audit->key); free(audit); (void)maelys_sys_fd_close(&fd);
        return MAELYS_EGRESS_ERR_MEMORY;
    }
    memcpy(audit->key, key, key_length);
    audit->key_length = key_length;
    audit->fd = fd;
    (void)snprintf(audit->key_id, sizeof(audit->key_id), "%s", key_id);
    memset(audit->chain_hex, '0', 64u);
    audit->chain_hex[64] = '\0';
    atomic_init(&audit->references, 1u);
    atomic_init(&audit->healthy, 1);
    atomic_init(&audit->records, 0u);
    if (!resume_audit(audit, out_error)) {
        atomic_store(&audit->healthy, 0);
        maelys_egress_audit_release(audit);
        return MAELYS_EGRESS_ERR_CRYPTO;
    }
    *out_audit = audit;
    return MAELYS_EGRESS_OK;
}

void maelys_egress_audit_retain(maelys_egress_audit_t *audit) {
    if (audit) (void)atomic_fetch_add(&audit->references, 1u);
}

void maelys_egress_audit_release(maelys_egress_audit_t *audit) {
    if (!audit || atomic_fetch_sub(&audit->references, 1u) != 1u) return;
    (void)pthread_mutex_lock(&audit->lock);
    (void)fdatasync(audit->fd);
    (void)flock(audit->fd, LOCK_UN);
    (void)maelys_sys_fd_close(&audit->fd);
    audit->fd = -1;
    egress_secure_zero(audit->key, audit->key_length);
    free(audit->key);
    (void)pthread_mutex_unlock(&audit->lock);
    (void)pthread_mutex_destroy(&audit->lock);
    free(audit);
}

uint64_t maelys_egress_audit_record_count(const maelys_egress_audit_t *audit) {
    return audit ? atomic_load(&audit->records) : 0u;
}

maelys_egress_result_t maelys_egress_audit_chain_copy(
    maelys_egress_audit_t *audit, char out_chain_hex[65]) {
    if (!audit || !out_chain_hex) return MAELYS_EGRESS_ERR_ARGUMENT;
    (void)pthread_mutex_lock(&audit->lock);
    memcpy(out_chain_hex, audit->chain_hex, 65u);
    (void)pthread_mutex_unlock(&audit->lock);
    return MAELYS_EGRESS_OK;
}

int maelys_egress_audit_healthy(const maelys_egress_audit_t *audit) {
    return audit && atomic_load(&audit->healthy);
}

static int write_all(int fd, const char *bytes, size_t length) {
    while (length) {
        ssize_t amount = write(fd, bytes, length);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return 0;
        bytes += (size_t)amount;
        length -= (size_t)amount;
    }
    return 1;
}

int egress_audit_append(maelys_egress_audit_t *audit,
                      const maelys_egress_receipt_t *receipt) {
    if (!audit || !receipt || !atomic_load(&audit->healthy)) return 0;
    (void)pthread_mutex_lock(&audit->lock);
    char canonical[2048];
    char core[2048];
    uint64_t sequence = atomic_load(&audit->records) + 1u;
    int core_length = egress_receipt_canonical(receipt, core, sizeof(core));
    int needed = core_length < 0 ? -1 : snprintf(canonical, sizeof(canonical),
        "seq=%llu|%s|attestor=%s|attestation_key=%s|attestation=%s|prev=%s",
        (unsigned long long)sequence, core, receipt->attestor,
        receipt->attestation_key_id, receipt->attestation_hex, audit->chain_hex);
    int ok = needed > 0 && (size_t)needed < sizeof(canonical);
    unsigned char mac[32];
    char mac_hex[65];
    if (ok) {
        ok = egress_hmac_sha256(audit->key, audit->key_length,
                         canonical, (size_t)needed, mac);
        if (ok) hex_encode(mac, sizeof(mac), mac_hex);
    }
    char record[3072];
    if (ok) {
        needed = snprintf(record, sizeof(record),
            "{\"v\":1,\"seq\":%llu,\"key_id\":\"%s\",\"previous\":\"%s\","
            "\"mac\":\"%s\",\"canonical\":\"%s\"}\n",
            (unsigned long long)sequence, audit->key_id, audit->chain_hex, mac_hex,
            canonical);
        ok = needed > 0 && (size_t)needed < sizeof(record) &&
            write_all(audit->fd, record, (size_t)needed) && fdatasync(audit->fd) == 0;
    }
    if (ok) {
        atomic_store(&audit->records, sequence);
        memcpy(audit->chain_hex, mac_hex, sizeof(audit->chain_hex));
    } else {
        atomic_store(&audit->healthy, 0);
    }
    egress_secure_zero(mac, sizeof(mac));
    (void)pthread_mutex_unlock(&audit->lock);
    return ok;
}
