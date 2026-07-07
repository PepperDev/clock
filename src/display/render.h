#ifndef RENDER_H
#define RENDER_H

#include "main.h"
#include "monitor/monitor.h"
#include <time.h>               // cppcheck-suppress missingIncludeSystem

enum { RENDER_BUF = 2048 };

void do_render(enum mode m, struct display *d, const struct tm *tm, const struct clock_state *ci, int once);
void set_date_str(struct clock_state *ci, const struct tm *tm);
unsigned long long wait_next_tick(void);
int render_panel_buf(char *buf, int sz, const struct clock_state *ci);
int fmt_widget_list(char *b, int z, const struct clock_state *ci, const char *pfx);
void emit_widget_panel(struct display *d, const struct clock_state *ci, int once, int sidebar_h);

#endif
