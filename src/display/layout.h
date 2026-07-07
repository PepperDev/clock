#ifndef LAYOUT_H
#define LAYOUT_H

#include "main.h"

enum { FALLBACK_CELL_W = 5, FALLBACK_CELL_H = 9 };

struct widget_ctx;
void recalc_size(unsigned short *row, unsigned short *col, int *size, unsigned short wsrow, int left_w);
void position_cursor(unsigned short row);
void layout_for_info(struct display *d, const struct widget_ctx *w);
void check_resize(struct display *d, const struct widget_ctx *w);
void read_startup_winsize(enum mode m, struct display *d);

#endif
