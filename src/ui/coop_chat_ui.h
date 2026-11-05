#ifndef ARCANUM_UI_COOP_CHAT_UI_H_
#define ARCANUM_UI_COOP_CHAT_UI_H_

#include "game/context.h"

bool coop_chat_ui_init(GameInitInfo* init_info);
void coop_chat_ui_exit(void);
void coop_chat_ui_reset(void);
void coop_chat_ui_resize(GameResizeInfo* resize_info);

#endif /* ARCANUM_UI_COOP_CHAT_UI_H_ */
