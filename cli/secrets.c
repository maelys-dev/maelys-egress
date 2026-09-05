#include "cli/cli.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Owner-only secret files: the proxy token and the audit HMAC key. Both are
 * read through the framework's trusted reader, which judges the descriptor it
 * reads: regular, not a symbolic link, owned by the caller only, one hard
 * link, no group or world bit, and bounded by the bytes actually read. */

#define EGRESS_CLI_SECRET_REQUIREMENTS \
    (MAELYS_CLI_FILE_REGULAR | MAELYS_CLI_FILE_NO_SYMLINK | \
     MAELYS_CLI_FILE_OWNER_CALLER | MAELYS_CLI_FILE_SINGLE_LINK | \
     MAELYS_CLI_FILE_PRIVATE)

void egress_cli_secure_zero(void *data, size_t length) {
    maelys_cli_zero(data, length);
}

int egress_cli_read_secret(const char *path, char secret[256]) {
    unsigned char *bytes = NULL;
    size_t length = 0u;
    if (maelys_cli_read_trusted_file(path, EGRESS_CLI_SECRET_REQUIREMENTS,
                                     16u, 255u, &bytes, &length, NULL) != 0) {
        return 0;
    }
    while (length > 0u && (bytes[length - 1u] == '\n' || bytes[length - 1u] == '\r')) {
        --length;
    }
    int ok = length >= 16u;
    if (ok) {
        memcpy(secret, bytes, length);
        secret[length] = '\0';
    } else {
        errno = EFBIG;
    }
    maelys_cli_zero(bytes, length);
    free(bytes);
    return ok;
}

int egress_cli_read_audit_key(
    const char *path, unsigned char **out_key, size_t *out_length) {
    if (!path || !out_key || !out_length) return 0;
    *out_key = NULL;
    *out_length = 0u;
    return maelys_cli_read_trusted_file(path, EGRESS_CLI_SECRET_REQUIREMENTS,
                                        16u, 4096u, out_key, out_length, NULL) == 0;
}
