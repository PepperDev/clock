#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <termios.h>            // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include "main.h"
#include "display/render.h"
#include "display/layout.h"
#include "display/calendar.h"
#include "monitor/monitor.h"
#include "monitor/monitor_int.h"
#include "monitor/widget.h"
#include "util/syscall.h"

static void setup_widget_data(struct clock_state *ci, const struct tm *tm, int text, int sun, int tty)
{
  if (widget_active(&ci->keep.widget, WIDGET_DATE))
    set_date_str(ci, tm);
  if (widget_active(&ci->keep.widget, WIDGET_CAL))
    set_cal_grid(ci, tm, text, sun, tty);
}

static const struct tm *tick_time(time_t tv_sec, struct tm *buf)
{
  localtime_r(&tv_sec, buf);
  return buf;
}

static void position_clock(enum mode m, const struct display *d, int once)
{
  if (m != MODE_TEXT && m != MODE_SIXEL && d->is_tty && !once)
    position_cursor(d->row);
}

static void render_and_wait(enum mode m, int once, struct display *d, const struct tm *tm, const struct clock_state *ci)
{
  position_clock(m, d, once);
  do_render(m, d, tm, ci, once);
}

static unsigned long long tick(enum mode m, int once, struct display *d, struct clock_state *ci, time_t now)
{
  struct tm tm_buf;
  const struct tm *tm = tick_time(now, &tm_buf);
  if (!once) {
    gather_all(ci, now);
    poll_async_fetches(ci, now);
    if (m != MODE_TEXT)
      check_resize(d, &ci->keep.widget);
  }
  if (widget_active(&ci->keep.widget, WIDGET_WEATHER))
    restore_weather(ci);
  setup_widget_data(ci, tm, m == MODE_TEXT, d->sunday_start, d->is_tty);
  render_and_wait(m, once, d, tm, ci);
  return once ? 0ULL : wait_next_tick();
}

static void clock_loop(enum mode m, int once, struct display *d, struct clock_state *c, unsigned long long t0)
{
  unsigned long long now = t0;
  if (once) {
    gather_all(c, (time_t) now);
    once_wait(c, now);
  }
  do {
    now = tick(m, once, d, c, now);
    if (tls_terminated)
      break;
  } while (!once);
  cleanup_all(d, c);
  if (d->is_tty)
    tcdrain(STDOUT_FILENO);
}

int clock_main(int argc, char **argv)
{
  struct args a;
  int r = parse_args(&a, argc, argv);
  if (r)
    return r < 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  struct clock_state c = { 0 };
  struct display d = {.sunday_start = a.sunday_start };
  unsigned long long t0 = (unsigned long long)time(NULL);
  clock_loop(setup_tty_and_mode(&a, &c, &d, t0), a.once, &d, &c, t0);
  return EXIT_SUCCESS;
}
