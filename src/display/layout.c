#include <stdio.h>              // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <sys/ioctl.h>          // cppcheck-suppress missingIncludeSystem
#include "main.h"
#include "display/layout.h"
#include "display/render.h"
#include "util/syscall.h"
#include "monitor/widget.h"
#include "display/esc.h"

enum { CUP_BUF = 16, IMPLICIT_MARGIN = 5, SIDEBAR_PCT = 30, SIDEBAR_MIN = 40, SIDEBAR_MAX = 60 };

// cppcheck-suppress staticFunction -- exposed for unit testing
void recalc_size(unsigned short *row, unsigned short *col, int *size, unsigned short wsrow, int left_w)
{
  int lines = wsrow / 16;
  int sz = left_w / (DOT_COLS + IMPLICIT_MARGIN);
  if (lines < sz)
    sz = lines;
  if (sz < 1)
    sz = 1;
  *size = sz;
  if (wsrow > 3)
    *row = (wsrow - (sz * DOT_ROWS + 1) / 2) / 2;
  else
    *row = 0;
  if (left_w > DOT_COLS) {
    int c = (left_w - sz * DOT_COLS) / 2;
    *col = c > 0 ? (unsigned short)c : 0;
  } else
    *col = 0;
}

static int calc_info_col(unsigned short wscol)
{
  int info_w = wscol * SIDEBAR_PCT / 100;
  if (info_w < SIDEBAR_MIN)
    info_w = SIDEBAR_MIN;
  if (info_w > SIDEBAR_MAX)
    info_w = SIDEBAR_MAX;
  int left_w = (int)wscol - info_w;
  if (left_w < 0)
    left_w = 0;
  return left_w;
}

static void update_layout(struct display *d, unsigned short wsrow, unsigned short wscol, const struct widget_ctx *w)
{
  d->wsrow = wsrow, d->wscol = wscol;
  int n;
  widget_get_active(w, &n);
  d->info_col = n ? calc_info_col(d->wscol) : d->wscol;
  recalc_size(&d->row, &d->col, &d->size, d->wsrow, d->info_col);
}

static int winsize_changed(const struct display *d, const struct winsize *ws)
{
  if (!ws->ws_row || !ws->ws_col)
    return 0;
  if (d->wsrow != ws->ws_row || d->wscol != ws->ws_col)
    return 1;
  return 0;
}

static void reset_screen(struct display *d)
{
  sys_write(STDOUT_FILENO, ESC_RIS, sizeof ESC_RIS - 1);
  d->cursor_hidden = 0;
  sys_write(STDOUT_FILENO, ESC_HIDE_CURSOR, sizeof ESC_HIDE_CURSOR - 1);
  d->cursor_hidden = 1;
}

void check_resize(struct display *d, const struct widget_ctx *w)
{
  if (!d->is_tty)
    return;
  if (!d->resized)
    return;
  d->resized = 0;
  struct winsize ws = { 0 };
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  if (!winsize_changed(d, &ws))
    return;
  reset_screen(d);
  update_layout(d, ws.ws_row, ws.ws_col, w);
  if (ws.ws_xpixel && ws.ws_ypixel) {
    d->cell_w = ws.ws_xpixel / ws.ws_col;
    d->cell_h = ws.ws_ypixel / ws.ws_row;
  }
}

void position_cursor(unsigned short row)
{
  char buf[CUP_BUF];
  int n = snprintf(buf, sizeof buf, "\033[%d;0H", row + 1);
  sys_write(STDOUT_FILENO, buf, n < (int)sizeof buf ? n : 0);
}

static int read_winsize(struct display *d)
{
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0)
    return -1;
  if (!ws.ws_row || !ws.ws_col)
    return -1;
  d->wsrow = ws.ws_row;
  d->wscol = ws.ws_col;
  if (ws.ws_xpixel && ws.ws_ypixel) {
    d->cell_w = ws.ws_xpixel / ws.ws_col;
    d->cell_h = ws.ws_ypixel / ws.ws_row;
  }
  return 0;
}

void read_startup_winsize(enum mode m, struct display *d)
{
  if (m == MODE_TEXT) {
    d->wsrow = 24, d->wscol = 80;
    return;
  }
  if (read_winsize(d) < 0)
    d->wsrow = 24, d->wscol = 80;
}

void layout_for_info(struct display *d, const struct widget_ctx *w)
{
  update_layout(d, d->wsrow, d->wscol, w);
}
