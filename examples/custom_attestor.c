#include <maelys/egress.h>

#include <stdint.h>
#include <stdio.h>

/* Demonstration only: replace with Ed25519, an HSM or a platform keystore. */
static maelys_egress_result_t example_sign(
    void *context,
    const void *canonical_receipt,
    size_t canonical_receipt_length,
    unsigned char *signature,
    size_t signature_capacity,
    size_t *out_signature_length,
    char **out_error) {
    (void)context;
    (void)out_error;
    if (!canonical_receipt || canonical_receipt_length == 0u ||
        !signature || signature_capacity < 4u || !out_signature_length) {
        return MAELYS_EGRESS_ERR_ARGUMENT;
    }
    signature[0] = (unsigned char)'D';
    signature[1] = (unsigned char)'E';
    signature[2] = (unsigned char)'M';
    signature[3] = (unsigned char)'O';
    *out_signature_length = 4u;
    return MAELYS_EGRESS_OK;
}

int main(void) {
    maelys_egress_attestor_t *attestor = NULL;
    char *error = NULL;
    maelys_egress_result_t result = maelys_egress_attestor_create(
        "example-provider", "example-key", 64u, example_sign,
        NULL, NULL, &attestor, &error);
    if (result != MAELYS_EGRESS_OK) {
        fprintf(stderr, "attestor create: %s\n",
                error ? error : maelys_egress_result_string(result));
    } else {
        puts("attestor provider created; install it with "
             "maelys_egress_config_set_receipt_attestor");
    }
    maelys_egress_error_free(error);
    maelys_egress_attestor_release(attestor);
    return result == MAELYS_EGRESS_OK ? 0 : 1;
}
