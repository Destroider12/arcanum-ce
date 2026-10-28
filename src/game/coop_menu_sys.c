#include "game/coop_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <tig/tig.h>

static int coop_menu_argc;
static char** coop_menu_argv;

void coop_menu_pick_address(char* out, size_t out_size)
{
    char* clip = SDL_GetClipboardText();

    coop_menu_pick_address_from(clip, out, out_size);
    if (clip != NULL) {
        SDL_free(clip);
    }
}

void coop_menu_set_args(int argc, char** argv)
{
    coop_menu_argc = argc;
    coop_menu_argv = argv;
}

static bool coop_menu_pending;
static bool coop_menu_pending_join;
static char coop_menu_pending_addr[96];

/* PT34: only records the request. The spawn happens at the very end of the
   shutdown (coop_menu_run_pending_relaunch), so the new process never meets
   this one's open log file or Save\\Current. */
bool coop_menu_relaunch(bool join, const char* addr)
{
    if (coop_menu_argc < 1 || coop_menu_argv == NULL || (join && !coop_menu_address_valid(addr))
        || (join && strlen(addr) >= sizeof(coop_menu_pending_addr))) {
        return false;
    }
    coop_menu_pending = true;
    coop_menu_pending_join = join;
    coop_menu_pending_addr[0] = '\0';
    if (join) {
        strcpy(coop_menu_pending_addr, addr);
    }
    return true;
}

void coop_menu_run_pending_relaunch(void)
{
    const char** args;
    int n = 0;
    int i;
    SDL_Process* process;
    bool join = coop_menu_pending_join;

    if (!coop_menu_pending) {
        return;
    }
    coop_menu_pending = false;
    args = (const char**)calloc((size_t)coop_menu_argc + 5, sizeof(*args));
    if (args == NULL) {
        return;
    }
    args[n++] = coop_menu_argv[0];
    for (i = 1; i < coop_menu_argc; i++) {
        const char* a = coop_menu_argv[i];

        if (SDL_strcasecmp(a, "-coop") == 0) {
            continue;
        }
        if (SDL_strcasecmp(a, "-host") == 0 || SDL_strcasecmp(a, "-join") == 0) {
            if (i + 1 < coop_menu_argc && coop_menu_argv[i + 1][0] != '-') {
                i++; /* its value */
            }
            continue;
        }
        args[n++] = a;
    }
    args[n++] = "-coop";
    args[n++] = join ? "-join" : "-host";
    if (join) {
        args[n++] = coop_menu_pending_addr;
    }
    args[n] = NULL;
    tig_debug_printf("CoopMenu: relaunching as co-op %s%s\n", join ? "client of " : "host",
        join ? coop_menu_pending_addr : "");
    /* Release the -log file: the new process opens the same path for
       writing. Nothing may log after this (a later write would reopen it). */
    tig_debug_set_log_file(NULL);
    process = SDL_CreateProcess(args, false);
    free((void*)args);
    if (process == NULL) {
        fprintf(stderr, "CoopMenu: relaunch failed: %s\n", SDL_GetError());
        return;
    }
    SDL_DestroyProcess(process);
}
