#ifndef ARCANUM_UI_MP_CTRL_UI_H_
#define ARCANUM_UI_MP_CTRL_UI_H_

#include "game/timeevent.h"

/* Restored stub: CE removed network UI but party.c still schedules
   TIMEEVENT_TYPE_MP_CTRL_UI. Process those events so timeevent_ping
   does not call a NULL function pointer. */
bool mp_ctrl_ui_process_callback(TimeEvent* timeevent);

#endif /* ARCANUM_UI_MP_CTRL_UI_H_ */
