/*
 * The standalone driver of the fuzz targets, used where libFuzzer is not
 * available: it feeds the committed seed corpus and whatever the target
 * hard-codes. Both gates then exercise the same seeds, so adding one file
 * strengthens the campaign and the smoke run at once.
 */
#ifndef MAELYS_FUZZ_STANDALONE_H
#define MAELYS_FUZZ_STANDALONE_H

#include <dirent.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* A seed is a small structured input; anything larger is not one. */
#define MAELYS_FUZZ_SEED_MAX (64u * 1024u)

static int fuzz_feed_file(const char *path) {
    FILE *stream = fopen(path, "rb");
    if (!stream) return 0;
    unsigned char *bytes = malloc(MAELYS_FUZZ_SEED_MAX);
    if (!bytes) { (void)fclose(stream); return 0; }
    size_t length = fread(bytes, 1u, MAELYS_FUZZ_SEED_MAX, stream);
    int oversized = !feof(stream);
    (void)fclose(stream);
    if (!oversized) (void)LLVMFuzzerTestOneInput(bytes, length);
    free(bytes);
    return oversized ? 0 : 1;
}

/* Returns the number of seeds fed, or -1 when the directory cannot be read:
 * a corpus that silently fed nothing would leave the gate reporting success
 * on an empty run. */
static int fuzz_feed_directory(const char *path) {
    DIR *directory = opendir(path);
    if (!directory) return -1;
    int fed = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char file[4096];
        int written = snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
        if (written <= 0 || (size_t)written >= sizeof(file)) continue;
        fed += fuzz_feed_file(file);
    }
    (void)closedir(directory);
    return fed;
}

static int fuzz_feed_arguments(int argc, char **argv) {
    for (int index = 1; index < argc; ++index) {
        int fed = fuzz_feed_directory(argv[index]);
        if (fed < 0) {
            (void)fprintf(stderr, "fuzz: cannot read the corpus %s\n", argv[index]);
            return 1;
        }
        if (fed == 0) {
            (void)fprintf(stderr, "fuzz: the corpus %s fed no seed\n", argv[index]);
            return 1;
        }
        (void)fprintf(stderr, "fuzz: %d seeds from %s\n", fed, argv[index]);
    }
    return 0;
}

#endif
