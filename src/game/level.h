#ifndef ARCANUM_GAME_LEVEL_H_
#define ARCANUM_GAME_LEVEL_H_

#include "game/context.h"

bool level_init(GameInitInfo* init_info);
void level_exit(void);
int level_experience_points_to_next_level(int64_t obj);
int level_xp_required(int level);
int level_progress_towards_next_level(int64_t obj);
void level_recalc(int64_t pc_obj);
int auto_level_scheme_get(int64_t obj);
int auto_level_scheme_set(int64_t obj, int value);
const char* auto_level_scheme_name(int scheme);
const char* auto_level_scheme_rule(int scheme);
void level_set(int64_t obj, int level);
/* Coop Middle (rule flvl): raise a follower to `level` the vanilla way
   (update_follower_level: level, character points, its auto-level scheme).
   Never lowers; capped below the level maximum. */
void level_follower_raise_to(int64_t follower_obj, int level);

#endif /* ARCANUM_GAME_LEVEL_H_ */
