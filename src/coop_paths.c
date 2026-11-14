#include "coop_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "tig/file.h"

static int coop_paths_cur_role;

static bool coop_paths_arg(int argc, char** argv, const char* name)
{
    size_t len = strlen(name);
    int i;

    for (i = 1; i < argc; i++) {
        if (SDL_strncasecmp(argv[i], name, len) == 0 && (argv[i][len] == '\0' || argv[i][len] == '=')) {
            return true;
        }
    }
    return false;
}

int coop_paths_role_from_args(int argc, char** argv)
{
    if (!coop_paths_arg(argc, argv, "-coop")) {
        return COOP_PATHS_ROLE_NONE;
    }
    if (coop_paths_arg(argc, argv, "-host")) {
        return COOP_PATHS_ROLE_HOST;
    }
    if (coop_paths_arg(argc, argv, "-join")) {
        return COOP_PATHS_ROLE_CLIENT;
    }
    return COOP_PATHS_ROLE_NONE;
}

void coop_paths_set_role(int role)
{
    coop_paths_cur_role = (role == COOP_PATHS_ROLE_HOST || role == COOP_PATHS_ROLE_CLIENT) ? role : COOP_PATHS_ROLE_NONE;
}

int coop_paths_role(void)
{
    return coop_paths_cur_role;
}

const char* coop_paths_dir(void)
{
    switch (coop_paths_cur_role) {
    case COOP_PATHS_ROLE_HOST:
        return "coop\\host";
    case COOP_PATHS_ROLE_CLIENT:
        return "coop\\client";
    }
    return "";
}

bool coop_paths_join(char* out, size_t size, const char* name)
{
    int n;

    if (coop_paths_cur_role == COOP_PATHS_ROLE_NONE || out == NULL || size == 0) {
        return false;
    }
#if defined(_WIN32)
    n = snprintf(out, size, "%s\\%s", coop_paths_dir(), name);
#else
    n = snprintf(out, size, "%s/%s", coop_paths_cur_role == COOP_PATHS_ROLE_HOST ? "coop/host" : "coop/client", name);
#endif
    return n > 0 && (size_t)n < size;
}

/* PT70: create coop\ and coop\<role>\ (parents first). */
bool coop_paths_ensure_dir(void)
{
    const char* dir;
    SDL_PathInfo info;

    if (coop_paths_cur_role == COOP_PATHS_ROLE_NONE) {
        return false;
    }
    dir = coop_paths_cur_role == COOP_PATHS_ROLE_HOST ? "coop/host" : "coop/client";
    SDL_CreateDirectory("coop");
    SDL_CreateDirectory(dir);
    return SDL_GetPathInfo(dir, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

bool coop_paths_mount(void)
{
    static const char* const private_prefixes[] = { "Save", "Players" };
    char path[64];

    if (coop_paths_cur_role == COOP_PATHS_ROLE_NONE) {
        return true;
    }
    snprintf(path, sizeof(path), ".\\%s", coop_paths_dir());
    if (!tig_file_set_overlay(path, private_prefixes, 2)) {
        return false;
    }
    /* PT77q: so tig_file_fopen("Players\\...", "wb") never depends on
       tig_file_is_directory("Players") seeing the game folder's Players. */
    tig_file_mkdir("Save");
    tig_file_mkdir("Players");
    return true;
}

static bool coop_paths_copy_file(const char* dst, const char* src)
{
    FILE* in;
    FILE* out;
    char buf[4096];
    size_t n;
    bool ok = true;

    in = fopen(src, "rb");
    if (in == NULL) {
        return false;
    }
    out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return false;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(out);
    return ok;
}

const char* coop_paths_cfg(const char* base_name)
{
    static char path[128];
    FILE* probe;

    if (!coop_paths_join(path, sizeof(path), base_name)) {
        return base_name;
    }
    probe = fopen(path, "rb");
    if (probe != NULL) {
        fclose(probe);
    } else {
        /* first coop run in this role: start from the player's options */
        if (!coop_paths_ensure_dir()) {
            return base_name; /* PT70: no role folder - old shared cfg */
        }
        coop_paths_copy_file(path, base_name);
    }
    return path;
}

static bool coop_paths_ru(void)
{
    const char* v = getenv("ARCANUM_COOP_LANG");

    return v != NULL && (v[0] == 'r' || v[0] == 'R') && (v[1] == 'u' || v[1] == 'U');
}

const char* coop_paths_save_prefix(void)
{
    bool ru = coop_paths_ru();

    switch (coop_paths_cur_role) {
    case COOP_PATHS_ROLE_HOST:
        return ru ? "[\xD0\xA5\xD0\xBE\xD1\x81\xD1\x82 / Host] " : "[Host] ";
    case COOP_PATHS_ROLE_CLIENT:
        return ru ? "[\xD0\x9A\xD0\xBB\xD0\xB8\xD0\xB5\xD0\xBD\xD1\x82 / Client] " : "[Client] ";
    }
    return "";
}

void coop_paths_save_description(const char* description, char* out, size_t size)
{
    const char* prefix = coop_paths_save_prefix();

    if (out == NULL || size == 0) {
        return;
    }
    if (description == NULL) {
        description = "";
    }
    if (prefix[0] == '\0' || description[0] == '[') {
        /* no role, or already tagged ("[Host] ...", re-saving a slot) */
        snprintf(out, size, "%s", description);
        return;
    }
    snprintf(out, size, "%s%s", prefix, description);
}
