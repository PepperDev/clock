#define _POSIX_C_SOURCE 199309L
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include <errno.h>              // cppcheck-suppress missingIncludeSystem
#include "main.h"
#include "display/render.h"
#include "display/draw.h"
#include "display/sixel.h"
#include "display/esc.h"
#include "monitor/widget.h"
#include "monitor/monitor_int.h"
#include "util/syscall.h"

enum { CUP_SZ = 32 };

void render_text_info(int, int, int, const struct clock_state *);

unsigned long long wait_next_tick(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  unsigned long long next = (unsigned long long)ts.tv_sec + 1;
  ts.tv_nsec = NS_PER_SEC - ts.tv_nsec;
  ts.tv_sec = 0;
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    if (tls_terminated)
      break;
  }
  return next;
}

static void position_sidebar(int once, int img_h)
{
  if (once) {
    char cup[CUP_SZ];
    int nl = snprintf(cup, sizeof cup, "\033[%dA", img_h);
    sys_write(STDOUT_FILENO, cup, nl);
  } else {
    position_cursor(0);
  }
}

void emit_widget_panel(struct display *d, const struct clock_state *ci, int once, int sidebar_h)
{
  position_sidebar(once, sidebar_h);
  char pbuf[RENDER_BUF];
  int n = render_panel_buf(pbuf, sizeof pbuf, ci);
  pbuf[n] = 0;
  int info_w = d->wscol > d->info_col ? d->wscol - d->info_col : 0;
  char col_buf[16];
  snprintf(col_buf, sizeof col_buf, "\033[%dG", d->info_col + 1);
  int lines = render_wrapped(pbuf, info_w, d->is_tty, col_buf);
  update_sidebar_lines(d, lines, once, d->is_tty, col_buf);
}

static void tty_widgets(struct display *d, const struct clock_state *ci, int once)
{
  emit_widget_panel(d, ci, once, (d->size * DOT_ROWS + 1) / 2);
}

static void write_widgets(struct display *d, const struct clock_state *ci, int once)
{
  if (d->is_tty) {
    tty_widgets(d, ci, once);
    return;
  }
  char pbuf[RENDER_BUF];
  int n = fmt_widget_list(pbuf, sizeof pbuf, ci, "");
  pbuf[n] = 0;
  sys_write(STDOUT_FILENO, pbuf, n);
}

static void render_ascii_mode(struct display *d, const struct tm *tm, const struct clock_state *ci, int once)
{
  struct draw_ctx dc = {.col = d->col,.tty = d->is_tty };
  render_ascii(&dc, tm->tm_hour, tm->tm_min, tm->tm_sec, d->size);
  int wc;
  widget_get_active(&ci->keep.widget, &wc);
  if (wc > 0)
    write_widgets(d, ci, once);
}

void do_render(enum mode m, struct display *d, const struct tm *tm, const struct clock_state *ci, int once)
{
  if (m == MODE_TEXT) {
    render_text_info(tm->tm_hour, tm->tm_min, tm->tm_sec, ci);
  } else if (m == MODE_SIXEL) {
    render_sixel(d, tm, ci, once);
  } else {
    render_ascii_mode(d, tm, ci, once);
  }
  /* modes already end with \n; once needs no extra, continuous needs one \n to separate frames */
  if (!once)
    sys_write(STDOUT_FILENO, "\n", sizeof "\n" - 1);
}
