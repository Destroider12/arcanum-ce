#ifndef ARCANUM_UI_COOP_MIDDLE_UI_H_
#define ARCANUM_UI_COOP_MIDDLE_UI_H_

/*
 * Coop Middle M2 UI: direct trade entry points, teammate status badges,
 * Alt-hold loot labels and the F9 net HUD. Co-op sessions only: every
 * entry point returns at once in single player.
 */

#include <stdbool.h>
#include <stdint.h>

#include "game/context.h"
#include "tig/types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool coop_middle_ui_init(GameInitInfo* init_info);
void coop_middle_ui_exit(void);
void coop_middle_ui_reset(void);
void coop_middle_ui_resize(GameResizeInfo* resize_info);

/* Trade with another player's PC: Ctrl+LMB in the world (from_portrait =
   false) or RMB on a teammate portrait. True = handled (window, or a
   notice); false = not a trade target, the click goes on as usual. */
bool coop_middle_ui_trade_try(int64_t target, bool from_portrait);

/* Status badge (14x14, top-right) on a teammate portrait window that is
   width pixels wide; draws nothing when the teammate has no status. */
void coop_middle_ui_draw_badge(tig_window_handle_t window_handle, int64_t obj, int width);

/* Iso overlay (called from the co-op chat overlay hook, after the map,
   objects and text floaters): Alt loot labels + F9 net HUD. */
void coop_middle_ui_draw(GameDrawInfo* draw_info);
void coop_middle_ui_on_scroll(int dx, int dy);

/* F9: net HUD on / off. False outside a co-op session (key goes on). */
bool coop_middle_ui_hud_toggle(void);

#ifdef __cplusplus
}
#endif

#endif /* ARCANUM_UI_COOP_MIDDLE_UI_H_ */
