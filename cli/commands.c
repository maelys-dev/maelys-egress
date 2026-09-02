#include "cli/cli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Command handlers. Each one consumes the validated invocation, calls the
 * product code and reports through the framework: one envelope on stdout
 * for finite commands, the lifecycle stream for `serve`. */

typedef struct text_buffer {
    char *data;
    size_t size;
    size_t capacity;
    int failed;
} text_buffer_t;

static void text_append(text_buffer_t *buffer, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void text_append(text_buffer_t *buffer, const char *format, ...) {
    if (buffer->failed) return;
    for (;;) {
        va_list arguments;
        va_start(arguments, format);
        size_t room = buffer->capacity - buffer->size;
        int written = vsnprintf(buffer->data ? buffer->data + buffer->size : NULL,
                                room, format, arguments);
        va_end(arguments);
        if (written < 0) { buffer->failed = 1; return; }
        if (buffer->data && (size_t)written < room) {
            buffer->size += (size_t)written;
            return;
        }
        size_t needed = buffer->size + (size_t)written + 1u;
        size_t grown = buffer->capacity ? buffer->capacity : 1024u;
        while (grown < needed) grown *= 2u;
        char *data = realloc(buffer->data, grown);
        if (!data) { buffer->failed = 1; return; }
        buffer->data = data;
        buffer->capacity = grown;
    }
}

static const char *text_value(text_buffer_t *buffer) {
    return buffer->failed || !buffer->data ? "" : buffer->data;
}

static void text_release(text_buffer_t *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

/* ---- config describe ------------------------------------------------------- */

static void describe_key(
    maelys_cli_json_writer_t *data, const egress_cli_config_spec_t *spec) {
    (void)maelys_cli_json_begin_object(data);
    (void)maelys_cli_json_key_string(data, "name", spec->name);
    (void)maelys_cli_json_key_string(data, "type",
        egress_cli_config_type_name(spec->type));
    (void)maelys_cli_json_key_boolean(data, "required", spec->required);
    (void)maelys_cli_json_key_boolean(data, "repeatable", spec->repeatable);
    (void)maelys_cli_json_key_boolean(data, "secret", spec->secret);
    if (spec->default_value) {
        (void)maelys_cli_json_key_string(data, "default", spec->default_value);
    }
    if (spec->range) (void)maelys_cli_json_key_string(data, "range", spec->range);
    if (spec->allowed_values) {
        (void)maelys_cli_json_key_string(data, "allowedValues", spec->allowed_values);
    }
    if (spec->requires) (void)maelys_cli_json_key_string(data, "requires", spec->requires);
    if (spec->conflicts) {
        (void)maelys_cli_json_key_string(data, "conflicts", spec->conflicts);
    }
    (void)maelys_cli_json_key_string(data, "description", spec->description);
    (void)maelys_cli_json_end_object(data);
}

int egress_cli_command_config_describe(maelys_cli_context_t *context) {
    const int tls = egress_cli_tls_listener_supported();
    size_t count = 0u;
    const egress_cli_config_spec_t *specs = egress_cli_config_specs(&count);
    size_t constraint_count = 0u;
    const char *const *constraints = egress_cli_config_constraints(&constraint_count);

    maelys_cli_json_writer_t data;
    maelys_cli_json_writer_init(&data);
    (void)maelys_cli_json_begin_object(&data);
    (void)maelys_cli_json_key_unsigned(&data, "configurationSchemaVersion", 1u);
    (void)maelys_cli_json_key_string(&data, "grammar", "strict-key-value");
    (void)maelys_cli_json_key_boolean(&data, "tlsListener", tls);
    (void)maelys_cli_json_key(&data, "keys");
    (void)maelys_cli_json_begin_array(&data);
    for (size_t i = 0u; i < count; ++i) {
        if (specs[i].tls_only && !tls) continue;
        describe_key(&data, &specs[i]);
    }
    (void)maelys_cli_json_end_array(&data);
    (void)maelys_cli_json_key(&data, "constraints");
    (void)maelys_cli_json_begin_array(&data);
    for (size_t i = 0u; i < constraint_count; ++i) {
        (void)maelys_cli_json_string(&data, constraints[i]);
    }
    (void)maelys_cli_json_end_array(&data);
    (void)maelys_cli_json_end_object(&data);

    text_buffer_t human = {0};
    text_append(&human, "Maelys Egress configuration schema 1\n\n"
        "The file is strict 'key = value' text. Unknown and duplicate scalar keys fail.\n"
        "TLS listener keys: %s.\n\n", tls ? "available" : "absent from this binary");
    for (size_t i = 0u; i < count; ++i) {
        if (specs[i].tls_only && !tls) continue;
        text_append(&human, "%-28s %-12s %s%s\n", specs[i].name,
            egress_cli_config_type_name(specs[i].type), specs[i].description,
            specs[i].required ? " (required)" : "");
    }
    text_append(&human, "\nCross-key constraints:\n");
    for (size_t i = 0u; i < constraint_count; ++i) {
        text_append(&human, "  - %s\n", constraints[i]);
    }
    int status = maelys_cli_succeed_writer(context, &data, text_value(&human),
                                           MAELYS_CLI_EXIT_OK);
    text_release(&human);
    return status;
}

/* ---- config validate ------------------------------------------------------- */

static int validation_report(
    maelys_cli_context_t *context, const maelys_cli_error_t *violation,
    const char *digest) {
    maelys_cli_json_writer_t data;
    maelys_cli_json_writer_init(&data);
    (void)maelys_cli_json_begin_object(&data);
    (void)maelys_cli_json_key_boolean(&data, "valid", violation == NULL);
    (void)maelys_cli_json_key_unsigned(&data, "configurationSchemaVersion", 1u);
    if (!violation) {
        (void)maelys_cli_json_key(&data, "policy");
        (void)maelys_cli_json_begin_object(&data);
        (void)maelys_cli_json_key_string(&data, "algorithm", "sha256");
        (void)maelys_cli_json_key_string(&data, "digest", digest);
        (void)maelys_cli_json_end_object(&data);
    } else {
        (void)maelys_cli_json_key(&data, "diagnostics");
        (void)maelys_cli_json_begin_array(&data);
        (void)maelys_cli_json_begin_object(&data);
        (void)maelys_cli_json_key_string(&data, "code", violation->code);
        (void)maelys_cli_json_key_string(&data, "message", violation->message);
        if (violation->hint[0]) {
            (void)maelys_cli_json_key_string(&data, "hint", violation->hint);
        }
        (void)maelys_cli_json_end_object(&data);
        (void)maelys_cli_json_end_array(&data);
    }
    (void)maelys_cli_json_end_object(&data);
    text_buffer_t human = {0};
    if (!violation) {
        text_append(&human, "Configuration is valid. Policy SHA-256: %s\n", digest);
    } else {
        text_append(&human, "Configuration is invalid: [%s] %s\n",
                    violation->code, violation->message);
        if (violation->hint[0]) text_append(&human, "Hint: %s\n", violation->hint);
    }
    int status = maelys_cli_succeed_writer(context, &data, text_value(&human),
        violation ? MAELYS_CLI_EXIT_VIOLATIONS : MAELYS_CLI_EXIT_OK);
    text_release(&human);
    return status;
}

int egress_cli_command_config_validate(maelys_cli_context_t *context) {
    const char *path = maelys_cli_option(context, "config");
    egress_cli_settings_t settings;
    maelys_cli_error_t error;
    if (egress_cli_settings_load(path, &settings, &error) != 0) {
        /* The file itself could not be read: an execution failure. Its
         * content being wrong is a validation report. */
        if (strcmp(error.code, MAELYS_CLI_CODE_NOT_FOUND) == 0 ||
            strcmp(error.code, MAELYS_CLI_CODE_ACCESS_DENIED) == 0 ||
            strcmp(error.code, MAELYS_CLI_CODE_IO_FAILED) == 0) {
            return maelys_cli_fail_error(context, &error);
        }
        return validation_report(context, &error, NULL);
    }
    char digest[65] = {0};
    int status = egress_cli_run(&settings, NULL, 1, &error, digest);
    egress_cli_settings_destroy(&settings);
    if (status != 0) return validation_report(context, &error, NULL);
    return validation_report(context, NULL, digest);
}

/* ---- serve ------------------------------------------------------------------ */

int egress_cli_command_serve(maelys_cli_context_t *context) {
    const char *path = maelys_cli_option(context, "config");
    egress_cli_settings_t settings;
    maelys_cli_error_t error;
    if (egress_cli_settings_load(path, &settings, &error) != 0) {
        return maelys_cli_fail_error(context, &error);
    }
    int status = egress_cli_run(&settings, path, 0, &error, NULL);
    egress_cli_settings_destroy(&settings);
    if (status < 0) return maelys_cli_fail_error(context, &error);
    /* 0: clean stop. 1: a fatal event was already written to the stream. */
    return status == 0 ? MAELYS_CLI_EXIT_OK : MAELYS_CLI_EXIT_FAILURE;
}

/* ---- completion ------------------------------------------------------------- */

static const char *const completion_shells[] = {"bash", "zsh", "fish", NULL};

static void completion_words(const maelys_cli_context_t *context, text_buffer_t *words) {
    size_t builtin_count = 0u;
    const maelys_cli_command_t *builtin = maelys_cli_builtin_commands(&builtin_count);
    const maelys_cli_command_t *groups[2] = {builtin, context->app->commands};
    size_t counts[2] = {builtin_count, context->app->command_count};
    for (size_t g = 0u; g < 2u; ++g) {
        for (size_t i = 0u; i < counts[g]; ++i) {
            const maelys_cli_command_t *command = &groups[g][i];
            if (command->hidden) continue;
            text_append(words, "%s ", command->pattern);
            for (size_t j = 0u; j < command->option_count; ++j) {
                text_append(words, "--%s ", command->options[j].name);
            }
        }
    }
    text_append(words, "--format --json --compact --pretty --non-interactive "
                       "--color --help --summary text json jsonl");
    for (size_t i = 0u; completion_shells[i]; ++i) {
        text_append(words, " %s", completion_shells[i]);
    }
}

int egress_cli_command_completion(maelys_cli_context_t *context) {
    const char *shell = maelys_cli_operand(context, 0u);
    size_t index = 0u;
    if (maelys_cli_parse_choice(shell, completion_shells, &index) != 0) {
        return maelys_cli_fail(context, MAELYS_CLI_CODE_VALIDATION_FAILED,
            "Use bash, zsh or fish.", "Unsupported completion shell: %s.", shell);
    }
    text_buffer_t words = {0};
    completion_words(context, &words);
    text_buffer_t script = {0};
    if (index == 0u) {
        text_append(&script, "_maelys_egress(){ local cur=${COMP_WORDS[COMP_CWORD]}; "
            "COMPREPLY=($(compgen -W '%s' -- \"$cur\")); }\n"
            "complete -F _maelys_egress maelys-egress\n", text_value(&words));
    } else if (index == 1u) {
        text_append(&script, "#compdef maelys-egress\n_arguments '*:word:(%s)'\n",
                    text_value(&words));
    } else {
        text_append(&script, "complete -c maelys-egress -f -a '%s'\n",
                    text_value(&words));
    }
    maelys_cli_json_writer_t data;
    maelys_cli_json_writer_init(&data);
    (void)maelys_cli_json_begin_object(&data);
    (void)maelys_cli_json_key_string(&data, "shell", completion_shells[index]);
    (void)maelys_cli_json_key_string(&data, "script", text_value(&script));
    (void)maelys_cli_json_end_object(&data);
    int status = maelys_cli_succeed_writer(context, &data, text_value(&script),
                                           MAELYS_CLI_EXIT_OK);
    text_release(&script);
    text_release(&words);
    return status;
}
