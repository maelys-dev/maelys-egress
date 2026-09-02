#include "cli/cli.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* The maelys-egress-lifecycle/1 JSON Lines stream owned by `serve`. Every
 * event is built with the framework JSON writer, so the bytes on stdout are
 * always well-formed JSON, and written as one line under one mutex so
 * receipts from the reactor thread and reload events from the signal
 * thread never interleave. No other CLI file writes to stdout. */

#define LIFECYCLE_CONTRACT "maelys-egress-lifecycle/1"

/* Receipt accessors return NULL for absent identities; the stream contract
 * carries every string field, empty rather than null. */
static const char *text_or_empty(const char *value) {
    return value ? value : "";
}

static pthread_mutex_t lifecycle_output_mutex = PTHREAD_MUTEX_INITIALIZER;

static void event_begin(maelys_cli_json_writer_t *writer, const char *event) {
    maelys_cli_json_writer_init(writer);
    (void)maelys_cli_json_begin_object(writer);
    (void)maelys_cli_json_key_unsigned(writer, "schemaVersion", 1u);
    (void)maelys_cli_json_key_string(writer, "contract", LIFECYCLE_CONTRACT);
    (void)maelys_cli_json_key_string(writer, "event", event);
}

static void event_end(maelys_cli_json_writer_t *writer) {
    (void)maelys_cli_json_end_object(writer);
    char *text = maelys_cli_json_finish(writer);
    (void)pthread_mutex_lock(&lifecycle_output_mutex);
    if (text) {
        fputs(text, stdout);
    } else {
        /* Allocation failure: still emit one parseable line so a consumer
         * waiting on the stream is not left hanging. */
        fputs("{\"schemaVersion\":1,\"contract\":\"" LIFECYCLE_CONTRACT "\","
              "\"event\":\"fatal\",\"message\":\"lifecycle event allocation failed\"}",
              stdout);
    }
    fputc('\n', stdout);
    fflush(stdout);
    (void)pthread_mutex_unlock(&lifecycle_output_mutex);
    free(text);
}

void egress_cli_lifecycle_message(const char *event, const char *message) {
    maelys_cli_json_writer_t writer;
    event_begin(&writer, event);
    (void)maelys_cli_json_key_string(&writer, "message", message ? message : "");
    event_end(&writer);
}

void egress_cli_lifecycle_ready(
    const char *unix_path, const char *tcp_host, uint16_t tcp_port,
    const char *admin_host, uint16_t admin_port, const char *policy_digest) {
    maelys_cli_json_writer_t writer;
    event_begin(&writer, "ready");
    (void)maelys_cli_json_key(&writer, "proxy");
    (void)maelys_cli_json_begin_object(&writer);
    if (unix_path) {
        (void)maelys_cli_json_key_string(&writer, "transport", "unix");
        (void)maelys_cli_json_key_string(&writer, "path", unix_path);
    } else {
        (void)maelys_cli_json_key_string(&writer, "transport", "tcp");
        (void)maelys_cli_json_key_string(&writer, "host", tcp_host);
        (void)maelys_cli_json_key_unsigned(&writer, "port", tcp_port);
    }
    (void)maelys_cli_json_end_object(&writer);
    (void)maelys_cli_json_key(&writer, "admin");
    if (admin_port) {
        (void)maelys_cli_json_begin_object(&writer);
        (void)maelys_cli_json_key_string(&writer, "host", admin_host);
        (void)maelys_cli_json_key_unsigned(&writer, "port", admin_port);
        (void)maelys_cli_json_end_object(&writer);
    } else {
        (void)maelys_cli_json_null(&writer);
    }
    (void)maelys_cli_json_key(&writer, "policy");
    (void)maelys_cli_json_begin_object(&writer);
    (void)maelys_cli_json_key_unsigned(&writer, "generation", 1u);
    (void)maelys_cli_json_key_string(&writer, "algorithm", "sha256");
    (void)maelys_cli_json_key_string(&writer, "digest", policy_digest);
    (void)maelys_cli_json_end_object(&writer);
    event_end(&writer);
}

void egress_cli_lifecycle_policy_reloaded(uint64_t generation, const char *digest) {
    maelys_cli_json_writer_t writer;
    event_begin(&writer, "policy-reloaded");
    (void)maelys_cli_json_key(&writer, "policy");
    (void)maelys_cli_json_begin_object(&writer);
    (void)maelys_cli_json_key_unsigned(&writer, "generation", generation);
    (void)maelys_cli_json_key_string(&writer, "digest", digest);
    (void)maelys_cli_json_end_object(&writer);
    event_end(&writer);
}

void egress_cli_receipt_sink(void *context, const maelys_egress_receipt_t *receipt) {
    (void)context;
    maelys_cli_json_writer_t writer;
    event_begin(&writer, "receipt");
    (void)maelys_cli_json_key(&writer, "receipt");
    (void)maelys_cli_json_begin_object(&writer);
    (void)maelys_cli_json_key_unsigned(&writer, "id", maelys_egress_receipt_id(receipt));
    (void)maelys_cli_json_key_integer(&writer, "protocol",
        (int64_t)maelys_egress_receipt_protocol(receipt));
    (void)maelys_cli_json_key_string(&writer, "host", text_or_empty(maelys_egress_receipt_host(receipt)));
    (void)maelys_cli_json_key_unsigned(&writer, "port", maelys_egress_receipt_port(receipt));
    (void)maelys_cli_json_key_string(&writer, "result",
        maelys_egress_result_string(maelys_egress_receipt_result(receipt)));
    (void)maelys_cli_json_key_unsigned(&writer, "durationMs",
        maelys_egress_receipt_duration_ms(receipt));
    (void)maelys_cli_json_key_unsigned(&writer, "bytesFromClient",
        maelys_egress_receipt_bytes_from_client(receipt));
    (void)maelys_cli_json_key_unsigned(&writer, "bytesToClient",
        maelys_egress_receipt_bytes_to_client(receipt));
    (void)maelys_cli_json_key_string(&writer, "policyDigest",
        text_or_empty(maelys_egress_receipt_policy_digest_hex(receipt)));
    (void)maelys_cli_json_key_string(&writer, "invocationId",
        text_or_empty(maelys_egress_receipt_invocation_id(receipt)));
    (void)maelys_cli_json_key_string(&writer, "principal",
        text_or_empty(maelys_egress_receipt_principal(receipt)));
    (void)maelys_cli_json_key_unsigned(&writer, "policyGeneration",
        maelys_egress_receipt_policy_generation(receipt));
    (void)maelys_cli_json_key(&writer, "quota");
    (void)maelys_cli_json_begin_object(&writer);
    (void)maelys_cli_json_key_integer(&writer, "closedScope",
        (int64_t)maelys_egress_receipt_quota_scope(receipt));
    (void)maelys_cli_json_key_unsigned(&writer, "connectionMax",
        maelys_egress_receipt_quota_connection_max_bytes(receipt));
    (void)maelys_cli_json_key_unsigned(&writer, "executionMax",
        maelys_egress_receipt_quota_execution_max_bytes(receipt));
    (void)maelys_cli_json_key_unsigned(&writer, "connectionObserved",
        maelys_egress_receipt_quota_connection_observed_bytes(receipt));
    (void)maelys_cli_json_key_unsigned(&writer, "executionBefore",
        maelys_egress_receipt_quota_execution_before_bytes(receipt));
    (void)maelys_cli_json_key_unsigned(&writer, "executionAfter",
        maelys_egress_receipt_quota_execution_after_bytes(receipt));
    (void)maelys_cli_json_end_object(&writer);
    (void)maelys_cli_json_key_string(&writer, "attestor",
        text_or_empty(maelys_egress_receipt_attestor(receipt)));
    (void)maelys_cli_json_key_string(&writer, "attestationKeyId",
        text_or_empty(maelys_egress_receipt_attestation_key_id(receipt)));
    (void)maelys_cli_json_key_string(&writer, "attestation",
        text_or_empty(maelys_egress_receipt_attestation_hex(receipt)));
    (void)maelys_cli_json_end_object(&writer);
    event_end(&writer);
}
