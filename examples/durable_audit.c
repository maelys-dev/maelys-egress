#include <maelys/egress.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    static const char key[] = "replace-with-a-secret-key-from-a-keystore";
    if (argc != 2) {
        fprintf(stderr, "usage: %s AUDIT.jsonl\n", argv[0]);
        return 2;
    }
    maelys_egress_audit_t *audit = NULL;
    char *error = NULL;
    maelys_egress_result_t result = maelys_egress_audit_file_create(
        argv[1], key, strlen(key), "example-key", &audit, &error);
    if (result != MAELYS_EGRESS_OK) {
        fprintf(stderr, "audit create: %s\n",
                error ? error : maelys_egress_result_string(result));
        maelys_egress_error_free(error);
        return 1;
    }
    char chain[65];
    result = maelys_egress_audit_chain_copy(audit, chain);
    if (result == MAELYS_EGRESS_OK) {
        printf("records=%llu chain=%s healthy=%d\n",
               (unsigned long long)maelys_egress_audit_record_count(audit),
               chain, maelys_egress_audit_healthy(audit));
    }
    maelys_egress_audit_release(audit);
    return result == MAELYS_EGRESS_OK ? 0 : 1;
}
