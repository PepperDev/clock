#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <signal.h>             // cppcheck-suppress missingIncludeSystem
#include <termios.h>            // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <errno.h>              // cppcheck-suppress missingIncludeSystem
#include <sys/ioctl.h>          // cppcheck-suppress missingIncludeSystem
#include "main.h"
#include "display/esc.h"
#include "display/layout.h"
#include "display/sixel.h"
#include "monitor/monitor.h"
#include "monitor/monitor_int.h"
#include "monitor/ioserv.h"
#include "util/syscall.h"

__thread volatile sig_atomic_t tls_terminated = 0;
__thread struct display *tls_display = NULL;

static void handler(int sig)
{
  if (sig == SIGWINCH) {
    if (tls_display && !tls_terminated)
      tls_display->resized = 1;
    return;
  }
  if (tls_terminated)
    _exit(128 + sig);
  tls_terminated = 1;
}

static void read_terminal_size(struct display *d)
{
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) || !ws.ws_row || !ws.ws_col)
    return;
  d->wsrow = ws.ws_row;
  d->wscol = ws.ws_col;
  if (ws.ws_xpixel && ws.ws_ypixel) {
    d->cell_w = ws.ws_xpixel / ws.ws_col;
    d->cell_h = ws.ws_ypixel / ws.ws_row;
  }
}

static void query_pixels_if_sixel(struct display *d)
{
  if (d->cell_w && d->cell_h)
    return;
  int wpix, hpix;
  if (sixel_query_pixels(d->is_tty, d->is_tty_stdin, &wpix, &hpix) || !wpix || !hpix)
    return;
  d->cell_w = wpix / d->wscol;
  d->cell_h = hpix / d->wsrow;
}

static void setup_sigaction(void)
{
  struct sigaction sa = {.sa_handler = handler,.sa_flags = SA_NODEFER };
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGWINCH, &sa, NULL);
}

static void setup_terminal(enum mode m, struct display *d)
{
  tls_display = d;
  sys_write(STDOUT_FILENO, ESC_RIS, sizeof ESC_RIS - 1);
  sys_write(STDOUT_FILENO, ESC_HIDE_CURSOR, sizeof ESC_HIDE_CURSOR - 1);
  d->cursor_hidden = 1;
  setup_sigaction();
  if (!d->is_tty_stdin)
    return;
  tcgetattr(STDIN_FILENO, &d->saved_termios);
  d->termios_valid = 1;
  struct termios raw = d->saved_termios;
  raw.c_lflag &= ~ECHO;
  if (m == MODE_SIXEL)
    raw.c_lflag &= ~ICANON;
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  read_terminal_size(d);
  if (m == MODE_SIXEL)
    query_pixels_if_sixel(d);
}

static enum mode resolve_auto(enum mode m, const struct display *d)
{
  if (m != MODE_AUTO || !d->is_tty)
    return m;
  return sixel_detect(d->is_tty, d->is_tty_stdin) ? MODE_SIXEL : MODE_ASCII;
}

void setup_if_tty(enum mode *m, int once, struct display *d)
{
  *m = resolve_auto(*m, d);
  if (!once && *m != MODE_TEXT && d->is_tty)
    setup_terminal(*m, d);
}

void setup_sixel_cleanup(enum mode m, struct display *d)
{
  if (m == MODE_SIXEL && d->is_tty)
    d->sixel_mode = 1;
}

static void cleanup_io(struct async_ctx *ctx)
{
  if (!(ctx->widget_mask & ((1u << WIDGET_NET) | (1u << WIDGET_WEATHER))))
    return;
  io_shutdown(&ctx->ioc);
  io_wait_stopped(&ctx->ioc);
}

static void cleanup_fds(struct clock_state *c)
{
  struct async_ctx *ctx = &c->keep.async;
  if (ctx->widget_mask & (1u << WIDGET_NET)) {
    dns_cancel(&ctx->wan4_dns);
    dns_cancel(&ctx->wan6_dns);
    rtnl_monitor_stop(&c->keep.rtnl_mon);
    sys_close(c->keep.net.dgram_fd);
    sys_close(c->keep.rtnl.fd);
    sys_close(c->keep.nlk.fd);
    free(c->keep.net.wcache.ifindices);
    free(c->keep.net.wcache.ssids);
  }
  if (ctx->widget_mask & (1u << WIDGET_WEATHER))
    dns_cancel(&ctx->weather_dns);
}

void cleanup_all(struct display *d, struct clock_state *c)
{
  cleanup_io(&c->keep.async);
  cleanup_fds(c);
  if (d->sixel_mode)
    sys_write(STDOUT_FILENO, ESC_ST, sizeof ESC_ST - 1);
  if (d->termios_valid)
    tcsetattr(STDIN_FILENO, TCSANOW, &d->saved_termios);
  if (!c->keep.text) {
    sys_write(STDOUT_FILENO, "\n", 1);
    if (d->cursor_hidden)
      sys_write(STDOUT_FILENO, ESC_SHOW_CURSOR, sizeof ESC_SHOW_CURSOR - 1);
  }
}
