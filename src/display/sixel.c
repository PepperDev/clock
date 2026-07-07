#define _GNU_SOURCE
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <poll.h>               // cppcheck-suppress missingIncludeSystem
#include <fcntl.h>              // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include <termios.h>            // cppcheck-suppress missingIncludeSystem
#include <sys/ioctl.h>          // cppcheck-suppress missingIncludeSystem
#include "display/sixel.h"
#include "display/draw.h"
#include "display/layout.h"
#include "util/syscall.h"

enum { OUTER_W_NUM = 27, OUTER_W_DEN = 29, INNER_W_NUM = 5, INNER_W_DEN = 27,
  MAX_PCT = 85, MAX_PCT_DEN = 100, MIN_COLS = 60, MIN_ROWS = 12,
  PIXEL_RESP_SZ = 64, DA1_RESP_SZ = 128, RUN_BUF = 32, SHORT_RUN_BUF = 3, DA1_POLL_MS = 10,
};

static int da1_read(int fd, char *buf, int sz)
{
  struct pollfd pfd = {.fd = fd,.events = POLLIN };
  if (sys_poll(&pfd, 1, DA1_POLL_MS) <= 0)
    return 0;
  int n = (int)sys_read(fd, buf, sz - 1);
  if (n > 0)
    buf[n] = 0;
  return n > 0 ? n : 0;
}

static void da1_setup(int fd, struct termios *save)
{
  struct termios t;
  tcgetattr(fd, save);
  t = *save;
  t.c_lflag &= ~(ICANON | ECHO);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr(fd, TCSADRAIN, &t);
}

// cppcheck-suppress staticFunction -- used from tests
int sixel_da1_parse(const char *buf)
{
  if (!buf)
    return 0;
  const char *p = strstr(buf, "\033[?");
  if (!p)
    return 0;
  return strstr(buf, ";4;") || strstr(buf, ";4c") || !strncmp(p + 3, "4;", 2)
      || !strncmp(p + 3, "4c", 2);
}

// cppcheck-suppress staticFunction -- used from tests
int sixel_pixels_parse(const char *buf, int *wpix, int *hpix)
{
  if (!buf || !wpix || !hpix)
    return -1;
  const char *p = strstr(buf, "\033[4;");
  if (!p)
    return -1;
  int h, w;
  if (sscanf(p, "\033[4;%d;%dt", &h, &w) == 2) {
    *wpix = w;
    *hpix = h;
    return 0;
  }
  return -1;
}

static int da1_query(int fd, const char *query, size_t qlen, char *buf, size_t bufsz)
{
  struct termios save;
  da1_setup(fd, &save);
  int fl = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  sys_write(STDOUT_FILENO, query, qlen);
  tcdrain(STDOUT_FILENO);
  int n = da1_read(fd, buf, (int)bufsz);
  fcntl(fd, F_SETFL, fl);
  tcflush(fd, TCIFLUSH);
  tcsetattr(fd, TCSADRAIN, &save);
  return n;
}

int sixel_query_pixels(int is_tty_stdout, int is_tty_stdin, int *wpix, int *hpix)
{
  if (!is_tty_stdout || !is_tty_stdin)
    return -1;
  char buf[PIXEL_RESP_SZ];
  if (!da1_query(STDIN_FILENO, "\033[14t", 5, buf, sizeof buf))
    return -1;
  return sixel_pixels_parse(buf, wpix, hpix);
}

int sixel_detect(int is_tty_stdout, int is_tty_stdin)
{
  if (!is_tty_stdout || !is_tty_stdin)
    return 0;
  char buf[DA1_RESP_SZ];
  if (!da1_query(STDIN_FILENO, "\033[c", 3, buf, sizeof buf))
    return 0;
  return sixel_da1_parse(buf);
}

int px_val(const struct dots *d, int x, int row, int iw, int ih)
{
  if ((unsigned int)x >= (unsigned int)iw || (unsigned int)row >= (unsigned int)ih)
    return 0;
  int dc = (unsigned int)x * OUTER_W_NUM / iw;
  int dr = (unsigned int)row * INNER_W_NUM / ih;
  if (dc < DOT_COLS && dr < DOT_ROWS)
    return d->c[dc][dr];
  return 0;
}

void encode_run(int val, int count)
{
  char c = val + 0x3F;
  if (count >= 4) {
    char buf[RUN_BUF];
    int n = snprintf(buf, sizeof buf, "!%d%c", count, c);
    sys_write(STDOUT_FILENO, buf, n);
  } else {
    char buf[SHORT_RUN_BUF];
    for (int i = 0; i < count; i++)
      buf[i] = c;
    sys_write(STDOUT_FILENO, buf, count);
  }
}

void encode_band(int W, const int *vals)
{
  int x = 0;
  while (x < W) {
    int v = vals[x];
    int run = 1;
    while (x + run < W && vals[x + run] == v)
      run++;
    encode_run(v, run);
    x += run;
  }
}

static struct inner_dim compute_inner(int dw, int cw)
{
  struct inner_dim r;
  r.iw = dw * cw * OUTER_W_NUM / OUTER_W_DEN;
  r.ih = r.iw * INNER_W_NUM / INNER_W_DEN;
  return r;
}

static struct outer_rect compute_outer_size(int aw, int ah, int cw, int ch)
{
  struct outer_rect r = {.aw = aw,.ah = ah };
  if (aw * cw * 7 <= ah * ch * 29) {
    r.dw = aw;
    r.dh = ((aw * cw * 7 + ch - 1) / ch) / 29;
  } else {
    r.dw = ((ah * ch * 29 + cw - 1) / cw) / 7;
    r.dh = ah;
  }
  return r;
}

static struct outer_rect compute_outer(int info_col, int wsrow, int is_tty, int cw, int ch)
{
  if (!is_tty) {
    info_col = 80;
    wsrow = 24;
  }
  int aw = info_col < MIN_COLS ? MIN_COLS : info_col;
  aw = aw * MAX_PCT / MAX_PCT_DEN;
  int ah = wsrow < MIN_ROWS ? MIN_ROWS : wsrow;
  ah = ah * MAX_PCT / MAX_PCT_DEN;
  return compute_outer_size(aw, ah, cw, ch);
}

struct sixel_size sixel_compute_size(int info_col, int wsrow, int is_tty, int cw, int ch)
{
  if (!cw || !ch) {
    cw = FALLBACK_CELL_W;
    ch = FALLBACK_CELL_H;
  }
  struct outer_rect o = compute_outer(info_col, wsrow, is_tty, cw, ch);
  struct inner_dim n = compute_inner(o.dw, cw);
  struct sixel_size sz = {
    .aw = o.aw,.ah = o.ah,
    .dw = o.dw,.dh = o.dh,
    .iw = n.iw,.ih = n.ih,
    .cw = cw,.ch = ch,
  };
  return sz;
}
