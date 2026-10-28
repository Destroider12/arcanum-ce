/*
 * tig_file_is_empty_directory must treat a directory holding one or two
 * entries as non-empty on platforms whose native listing omits "." and
 * "..". (Linux: Save\Current holding only "maps" was never cleared, so a
 * coop client loaded stale map state with old PCs — Playtest 6.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tig/file.h"

static int check(const char* label, bool got, bool want)
{
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

int main(void)
{
    char root[] = "/tmp/tig_empty_dir_XXXXXX";
    char path[512];
    FILE* f;
    int rc = 0;

    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    rc |= check("fresh dir", tig_file_is_empty_directory(root), true);

    snprintf(path, sizeof(path), "%s/maps", root);
    mkdir(path, 0700);
    rc |= check("one subdir", tig_file_is_empty_directory(root), false);

    snprintf(path, sizeof(path), "%s/maps/mobile.mdy", root);
    f = fopen(path, "wb");
    fputs("x", f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/maps", root);
    rc |= check("dir with one file", tig_file_is_empty_directory(path), false);

    rc |= check("empty_directory ok", tig_file_empty_directory(root), true);
    rc |= check("after empty", tig_file_is_empty_directory(root), true);

    rmdir(root);
    if (rc == 0) {
        printf("tig_file_is_empty_directory OK\n");
    }
    return rc;
}
