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

#ifdef __cplusplus
}
#endif

#endif /* ARCANUM_UI_COOP_MIDDLE_UI_H_ */
