#ifndef ARCANUM_CRASH_HANDLER_H_
#define ARCANUM_CRASH_HANDLER_H_

/* PT56a: Windows crash reports (log + crash-<time>.txt + .dmp next to the
   exe). No-op elsewhere. Install once, early in main. */
void crash_handler_install(const char* build_id);
/* PT68: crash files go to <exe dir>\<subdir>\ (e.g. coop\host). */
void crash_handler_set_subdir(const char* subdir);
/* PT77af: the main loop is alive (gamelib_ping). Feeds the hang watchdog. */
void crash_handler_heartbeat(void);

#endif
