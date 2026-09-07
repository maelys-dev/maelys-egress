/*
 * A consumer of the published library, built the way an embedder builds:
 * the installed headers and the installed archive, found through
 * pkg-config alone. It proves the shipped artifact links and runs on its
 * own, so a dependency the library must not carry cannot hide behind the
 * repository's own build.
 *
 * Offline by construction: the sealed destination is a numeric loopback
 * address, so sealing pins it without asking a resolver.
 */
#include <maelys/egress.h>

#include <stdio.h>
#include <string.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "public consumer: %s failed at line %d\n", \
                          #condition, __LINE__); \
            return 1; \
        } \
    } while (0)

int main(void) {
    CHECK(maelys_egress_version_string() != NULL);
    CHECK(maelys_egress_version_string()[0] != '\0');
    CHECK(maelys_egress_abi_version() == MAELYS_EGRESS_ABI_VERSION);
    CHECK(maelys_egress_result_string(MAELYS_EGRESS_OK) != NULL);
    CHECK(maelys_egress_result_string(MAELYS_EGRESS_OK)[0] != '\0');

    maelys_egress_policy_t *policy = NULL;
    char *error = NULL;
    CHECK(maelys_egress_policy_create(&policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_allow_tcp(policy, "127.0.0.1", 9u, 1, &error) ==
          MAELYS_EGRESS_OK);
    CHECK(!maelys_egress_policy_is_sealed(policy));
    CHECK(maelys_egress_policy_seal(policy, &error) == MAELYS_EGRESS_OK);
    CHECK(maelys_egress_policy_is_sealed(policy));

    const char *digest = maelys_egress_policy_digest_hex(policy);
    CHECK(digest != NULL && strlen(digest) == 64u);
    for (size_t i = 0; i < 64u; ++i) {
        CHECK((digest[i] >= '0' && digest[i] <= '9') ||
              (digest[i] >= 'a' && digest[i] <= 'f'));
    }

    (void)printf("public consumer: linked from pkg-config, sealed %s\n", digest);
    maelys_egress_policy_destroy(policy);
    maelys_egress_error_free(error);
    return 0;
}
