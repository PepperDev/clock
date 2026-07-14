#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include "main.h"
#include "display/layout.h"
#include "monitor/monitor.h"
#include "monitor/monitor_int.h"
#include "monitor/widget.h"
#include "display/esc.h"
#include "util/syscall.h"

static void start_net_fetch(struct clock_state *c, unsigned long long t0, int once, int ip_refresh)
{
  if (!widget_active(&c->keep.widget, WIDGET_NET))
    return;
  c->keep.ip_refresh_sec = ip_refresh;
  c->keep.v4_ip_refresh_ts = t0;
  c->keep.v6_ip_refresh_ts = t0;
  if (once) {
    wan_dns_start(c, 0);
    wan_dns_start(c, 1);
  } else {
    if (rtnl_open_monitor(&c->keep.rtnl_mon) == 0)
      rtnl_monitor_start(&c->keep.rtnl_mon);
    wan_dns_start(c, 0);
    wan_dns_start(c, 1);
  }
}

static void start_weather_fetch(struct clock_state *c, unsigned long long t0, int weather_refresh)
{
  if (!widget_active(&c->keep.widget, WIDGET_WEATHER))
    return;
  c->keep.weather_refresh_sec = weather_refresh;
  c->keep.weather_refresh_ts = t0;
  weather_dns_start(c);
}

static void start_fetches(struct clock_state *c, unsigned long long t0, int once, int ip_refresh, int weather_refresh)
{
  start_net_fetch(c, t0, once, ip_refresh);
  start_weather_fetch(c, t0, weather_refresh);
}

// cppcheck-suppress staticFunction -- exposed for unit testing
enum mode resolve_mode(enum mode m, int tty)
{
  if (m == MODE_AUTO && !tty)
    return MODE_TEXT;
  return m;
}

static void init_clock(const struct args *a, struct clock_state *c, unsigned long long t0)
{
  struct cpu_keep *k = &c->keep;
  k->nlk.fd = -1;
  widget_setup(&k->widget, a->has_widgets, a->widgets);
  async_ctx_init(&k->async, &k->wan4_fd, &k->wan6_fd, &k->weather_fd, k->widget.active_mask);
  start_fetches(c, t0, a->once, a->ip_refresh, a->weather_refresh);
}

enum mode setup_tty_and_mode(const struct args *a, struct clock_state *c, struct display *d, unsigned long long t0)
{
  d->is_tty = sys_isatty(STDOUT_FILENO);
  d->is_tty_stdin = sys_isatty(STDIN_FILENO);
  enum mode m = resolve_mode(a->mode, d->is_tty);
  c->keep.text = (m == MODE_TEXT);
  init_clock(a, c, t0);
  read_startup_winsize(m, d);
  layout_for_info(d, &c->keep.widget);
  setup_if_tty(&m, a->once, d);
  if (d->is_tty && m == MODE_ASCII)
    d->is_tty |= TTY_EL;
  setup_sixel_cleanup(m, d);
  return m;
}
