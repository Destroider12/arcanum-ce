#ifndef ARCANUM_COOP_PATHS_H_
#define ARCANUM_COOP_PATHS_H_

#include <stdbool.h>
#include <stddef.h>

/* PT68: one game folder for single player, a coop host and a coop client
   (even at the same time on one PC). The coop role gets its own writable
   folder, coop\host\ or coop\client\, holding Save\, Players\, arcanum.cfg,
   the default log and crash files; game data stays shared. Role 0 (no coop,
   or -coop without -host/-join) changes nothing. */

#define COOP_PATHS_ROLE_NONE 0
#define COOP_PATHS_ROLE_HOST 1
#define COOP_PATHS_ROLE_CLIENT 2

/* From argv (whole tokens): -coop plus -host or -join. */
int coop_paths_role_from_args(int argc, char** argv);
void coop_paths_set_role(int role);
int coop_paths_role(void);

/* "coop\\host" / "coop\\client" (Windows separators), "" for no role. */
const char* coop_paths_dir(void);

/* dir + "\\" + name for plain fopen (separators as given; Windows and the
   SDL/POSIX layer both accept '/'). Returns false for no role. */
bool coop_paths_join(char* out, size_t size, const char* name);

/* Creates the folder and mounts it as the tig write overlay (Save and
   Players private to it). Call after tig_init. No-op for no role. */
bool coop_paths_ensure_dir(void);
bool coop_paths_mount(void);

/* Settings file: "arcanum.cfg" or coop\<role>\arcanum.cfg (seeded once from
   the root file so options carry over). Static storage. */
const char* coop_paths_cfg(const char* base_name);

/* "[Host] " / "[Client] " (ARCANUM_COOP_LANG=ru: "[Хост / Host] " ...) in
   front of a save description, unless it already starts with it. */
void coop_paths_save_description(const char* description, char* out, size_t size);
const char* coop_paths_save_prefix(void);

#endif
