/*
 * PT68: a coop role overlay (coop\host) on top of the module folder: every
 * write lands in the overlay, Save\ and Players\ resolve only inside it,
 * other files still read through to the shared folders.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tig/file.h"

static int fails;

static void check(const char* label, bool ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", label);
        fails++;
    }
}

static void put(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");

    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

static bool native_exists(const char* path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

int main(void)
{
    static const char* const priv[] = { "Save", "Players" };
    char root[] = "/tmp/tig_overlay_XXXXXX";
    TigFileList list;
    TigFile* f;
    char buf[32];

    if (mkdtemp(root) == NULL || chdir(root) != 0) {
        perror("tmp");
        return 1;
    }
    mkdir("mod", 0700);
    mkdir("mod/Save", 0700);
    mkdir("mod/maps", 0700);
    put("mod/Save/Slot0001.gsi", "single");
    put("mod/maps/shared.txt", "shared");

    check("add module dir", tig_file_repository_add("mod"));
    check("set overlay", tig_file_set_overlay(".\\coop\\host", priv, 2));
    check("overlay folder made", native_exists("coop/host"));

    /* shared data reads through */
    f = tig_file_fopen("maps\\shared.txt", "rb");
    check("shared read", f != NULL);
    if (f != NULL) {
        memset(buf, 0, sizeof(buf));
        tig_file_fread(buf, 1, sizeof(buf) - 1, f);
        tig_file_fclose(f);
        check("shared content", strcmp(buf, "shared") == 0);
    }

    /* single-player saves are invisible to the role */
    check("sp save hidden (exists)", !tig_file_exists("Save\\Slot0001.gsi", NULL));
    check("sp save hidden (open)", tig_file_fopen("Save\\Slot0001.gsi", "rb") == NULL);
    check("sp save hidden (./ path)", !tig_file_exists(".\\Save\\Slot0001.gsi", NULL));
    check("sp save hidden (./ open)", tig_file_fopen(".\\Save\\Slot0001.gsi", "rb") == NULL);
    tig_file_list_create(&list, "Save\\*.gsi");
    check("sp save hidden (list)", list.count == 0);
    tig_file_list_destroy(&list);

    /* role writes land in the overlay */
    check("mkdir Save", tig_file_mkdir("Save"));
    f = tig_file_fopen("Save\\Slot0002.gsi", "wb");
    check("role save open", f != NULL);
    if (f != NULL) {
        tig_file_fwrite("host", 1, 4, f);
        tig_file_fclose(f);
    }
    check("role save in overlay", native_exists("coop/host/Save/Slot0002.gsi"));
    check("role save not in module", !native_exists("mod/Save/Slot0002.gsi"));
    tig_file_list_create(&list, "Save\\*.gsi");
    check("role list sees only its save", list.count == 1);
    tig_file_list_destroy(&list);

    /* removing never reaches the shared folder */
    tig_file_remove("Save\\Slot0001.gsi");
    check("sp save still there", native_exists("mod/Save/Slot0001.gsi"));

    /* a later repository_add keeps the overlay on top */
    mkdir("data", 0700);
    tig_file_repository_add("data");
    f = tig_file_fopen("note.txt", "wb");
    if (f != NULL) {
        tig_file_fclose(f);
    }
    check("writes still go to overlay after add", native_exists("coop/host/note.txt"));

    /* off again: single player sees its own save */
    tig_file_set_overlay(NULL, NULL, 0);
    check("overlay off: sp save visible", tig_file_exists("Save\\Slot0001.gsi", NULL));

    tig_file_repository_remove_all();
    if (fails == 0) {
        printf("tig_file overlay OK\n");
    }
    return fails != 0;
}
