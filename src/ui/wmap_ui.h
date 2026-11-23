#ifndef ARCANUM_UI_WMAP_UI_H_
#define ARCANUM_UI_WMAP_UI_H_

#include "game/context.h"
#include "game/timeevent.h"

bool wmap_ui_init(GameInitInfo* init_info);
void wmap_ui_exit(void);
void wmap_ui_reset(void);
bool wmap_ui_save(TigFile* stream);
bool wmap_ui_load(GameLoadInfo* load_info);
void wmap_ui_encounter_start(void);
bool wmap_ui_is_encounter(void);
void wmap_ui_encounter_end(void);
void wmap_ui_open(void);
void wmap_ui_select(int64_t obj, int spell);
void wmap_ui_close(void);
int wmap_ui_is_created(void);
bool wmap_ui_is_town_mode(void);
void wmap_ui_scroll(int direction);
void sub_564000(int a1);
void wmap_ui_mark_townmap(int64_t obj);
bool wmap_ui_bkg_process_callback(TimeEvent* timeevent);
void wmap_ui_notify_sector_changed(int64_t pc_obj, int64_t sec);
void wmap_ui_get_current_location(int64_t* loc_ptr);

/* PT73: coop client watches the host's world map (no input). */
bool wmap_ui_spectate_open(void);
void wmap_ui_spectate_set(int x, int y);
void wmap_ui_spectate_close(void);
bool wmap_ui_is_spectating(void);
/* Host: party marker on the open world map (false otherwise). */
bool wmap_ui_world_coords_get(int* x, int* y);
bool wmap_ui_is_moving(void);

#endif /* ARCANUM_UI_WMAP_UI_H_ */
