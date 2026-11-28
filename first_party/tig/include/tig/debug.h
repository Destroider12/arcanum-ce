#ifndef TIG_DEBUG_H_
#define TIG_DEBUG_H_

#include "tig/types.h"

#ifdef __cplusplus
extern "C" {
#endif

int tig_debug_init(TigInitInfo* init_info);
void tig_debug_exit(void);
void tig_debug_printf(const char* format, ...);
void tig_debug_println(const char* string);
/* PT23: also write every log line to this file (NULL: ARCANUM_LOG_FILE). */
void tig_debug_set_log_file(const char* path);
const char* tig_debug_last_coop_line(void);
/* PT77p: i-th oldest of the last 64 co-op/travel log lines, NULL past the end. */
const char* tig_debug_breadcrumb(int index);
void tig_debug_crash_write(const char* str);

#ifdef __cplusplus
}
#endif

#endif /* TIG_DEBUG_H_ */
