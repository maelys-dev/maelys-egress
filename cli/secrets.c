#include "cli/cli.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Owner-only secret files: the proxy token and the audit HMAC key. */

void egress_cli_secure_zero(void *data, size_t length) {
    volatile unsigned char *bytes = data;
    while (length) { *bytes++ = 0u; --length; }
}

int egress_cli_read_secret(const char *path, char secret[256]) {
    struct stat status;
#ifdef O_NOFOLLOW
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC);
#endif
    if (fd < 0 || fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || status.st_nlink != 1u ||
        (status.st_mode & 077u) != 0u) {
        int saved = fd < 0 ? errno : EPERM;
        if (fd >= 0) (void)close(fd);
        errno = saved;
        return 0;
    }
    unsigned char bytes[256];
    ssize_t length;
    do { length = read(fd, bytes, sizeof(bytes)); }
    while (length < 0 && errno == EINTR);
    int saved = errno;
    (void)close(fd);
    errno = saved;
    if (length < 16 || length >= (ssize_t)sizeof(bytes)) {
        egress_cli_secure_zero(bytes, sizeof(bytes));
        return 0;
    }
    while (length > 0 && (bytes[length - 1] == '\n' || bytes[length - 1] == '\r')) {
        --length;
    }
    if (length < 16) {
        egress_cli_secure_zero(bytes, sizeof(bytes));
        return 0;
    }
    memcpy(secret, bytes, (size_t)length);
    secret[length] = '\0';
    egress_cli_secure_zero(bytes, sizeof(bytes));
    return 1;
}

int egress_cli_read_audit_key(
    const char *path, unsigned char **out_key, size_t *out_length) {
    if (!path || !out_key || !out_length) return 0;
    *out_key = NULL;
    *out_length = 0u;
#ifdef O_NOFOLLOW
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC);
#endif
    struct stat status;
    if (fd < 0 || fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || status.st_nlink != 1u ||
        (status.st_mode & 077u) != 0u || status.st_size < 16 ||
        status.st_size > 4096) {
        int saved = fd < 0 ? errno : EPERM;
        if (fd >= 0) (void)close(fd);
        errno = saved;
        return 0;
    }
    size_t length = (size_t)status.st_size;
    unsigned char *key = malloc(length);
    if (!key) { (void)close(fd); return 0; }
    size_t used = 0u;
    while (used < length) {
        ssize_t amount = read(fd, key + used, length - used);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) break;
        used += (size_t)amount;
    }
    int saved = errno;
    (void)close(fd);
    errno = saved;
    if (used != length) {
        egress_cli_secure_zero(key, length); free(key); return 0;
    }
    *out_key = key;
    *out_length = length;
    return 1;
}
