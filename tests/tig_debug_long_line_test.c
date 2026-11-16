/*
 * PT53c: tig_debug_printf with a partial (no '\n') line that overflows the
 * 1024-byte line buffer must neither read past it nor stall all later
 * logging. Checked through the log file (ARCANUM_LOG_FILE). Run under ASan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tig/debug.h"

int main(void)
{
    char path[] = "/tmp/tig_debug_long_XXXXXX";
    char chunk[700];
    char line[4096];
    FILE* f;
    int fd = mkstemp(path);
    int found = 0;

    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(fd);
    tig_debug_set_log_file(path);
    memset(chunk, 'x', sizeof(chunk) - 1);
    chunk[sizeof(chunk) - 1] = '\0';
    tig_debug_printf("%s", chunk); /* buffered: no newline */
    tig_debug_printf("%s", chunk); /* overflows the 1024-byte buffer */
    tig_debug_printf("%s", chunk);
    tig_debug_printf("after-overflow marker\n");
    tig_debug_printf("[CHAT] ok\n");
    tig_debug_printf("[CHAT] ok\n");
    tig_debug_set_log_file(NULL); /* closes the file */

    f = fopen(path, "r");
    if (f == NULL) {
        perror("fopen");
        return 1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "after-overflow marker") != NULL) {
            found++;
        }
        if (strstr(line, "[CHAT] ok") != NULL) {
            found++;
        }
    }
    fclose(f);
    unlink(path);
    if (found < 1) {
        fprintf(stderr, "logging stalled after an overflowing partial line\n");
        return 1;
    }
    if (found < 3) {
        fprintf(stderr, "identical [CHAT] lines were collapsed or dropped\n");
        return 1;
    }
    printf("tig_debug long partial line OK\n");
    return 0;
}
