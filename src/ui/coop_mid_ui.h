#ifndef ARCANUM_UI_COOP_MID_UI_H_
#define ARCANUM_UI_COOP_MID_UI_H_

#include "game/context.h"

/* Coop Middle overlay: Alt loot labels, pings, pause banner, F9 net HUD. */
bool coop_mid_ui_init(GameInitInfo* init_info);
void coop_mid_ui_exit(void);
void coop_mid_ui_reset(void);
void coop_mid_ui_resize(GameResizeInfo* resize_info);

#endif /* ARCANUM_UI_COOP_MID_UI_H_ */
