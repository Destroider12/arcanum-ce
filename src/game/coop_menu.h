#ifndef ARCANUM_GAME_COOP_MENU_H_
#define ARCANUM_GAME_COOP_MENU_H_

#include <stdbool.h>
#include <stddef.h>

/*
 * PT33 item 6: helpers for the main menu's "Host Co-op" / "Join Co-op"
 * entries. The co-op subsystem is configured once at start-up (-coop with
 * -host / -join), so the menu entries relaunch the game with those flags
 * instead of switching a running single-player session over.
 */

/* Host or host:port made of letters, digits, '.', '-', '_' and one ':port'. */
bool coop_menu_address_valid(const char* addr);

/* Trim, then validate; false when `text` is not an address. */
bool coop_menu_address_from_text(const char* text, char* out, size_t out_size);

/* The remembered address (coop_join.txt), else 127.0.0.1. */
void coop_menu_last_address(char* out, size_t out_size);

/* Remember `addr` as the last used join address. */
void coop_menu_remember_address(const char* addr);

/* Address a Join press would use right now: a valid host[:port] on the
   clipboard (the host's IP just copied) wins, else the remembered one. */
void coop_menu_pick_address(char* out, size_t out_size);

/* Same, with the clipboard text given (NULL = none). */
void coop_menu_pick_address_from(const char* clip, char* out, size_t out_size);

/* main(): keep argv for the relaunch. */
void coop_menu_set_args(int argc, char** argv);

/* Start a new copy of the game as host (join == false) or as a client of
   `addr`, keeping the other command-line options. True when it started. */
bool coop_menu_relaunch(bool join, const char* addr);

#endif /* ARCANUM_GAME_COOP_MENU_H_ */
