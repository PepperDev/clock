#include <string.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor/monitor.h"
#include "display/calendar.h"

static const char *MN[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const int DIM[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static const char *WD[] = { "Mo Tu We Th Fr Sa Su", "Su Mo Tu We Th Fr Sa" };

static int is_leap(int y)
{
  return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}

static void write_cal_grid(char *p, int d, int w, int mday, size_t rem)
{
  for (int i = 1; i <= d; i++) {
    int n = snprintf(p, rem, mday > 0 && i == mday ? "\033[7m%2d\033[27m " : "%2d ", i);
    if ((size_t)n >= rem)
      break;
    p += n, rem -= n;
    if ((w + i) % 7 == 0)
      *p++ = '\n', rem--;
  }
  *p = 0;
}

static int fmt_cal_header(char *b, size_t z, const struct tm *t, int w, int ss)
{
  return snprintf(b, z, "   %s %d\n%s\n%*s", MN[t->tm_mon], 1900 + t->tm_year, WD[ss], w * 3, "");
}

void set_cal_grid(struct clock_state *ci, const struct tm *t, int text, int ss, int tty)
{
  int d = DIM[t->tm_mon] + (t->tm_mon == 1) * is_leap(1900 + t->tm_year);
  struct tm x = *t;
  x.tm_mday = 1;
  mktime(&x);
  int w = (x.tm_wday + 6 * !ss) % 7, n = fmt_cal_header(ci->cal_grid, sizeof ci->cal_grid, t, w, ss);
  write_cal_grid(ci->cal_grid + n, d, w, !!tty * !text * t->tm_mday, sizeof ci->cal_grid - n);
}
