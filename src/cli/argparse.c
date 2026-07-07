#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stddef.h>             // cppcheck-suppress missingIncludeSystem
#include <errno.h>              // cppcheck-suppress missingIncludeSystem
#include <limits.h>             // cppcheck-suppress missingIncludeSystem
#include "main.h"
#include "util/syscall.h"
#include "argparse_int.h"
#include "monitor/widget.h"

static const char HELP_STR[] =
    "Usage: clock [OPTIONS] [auto|text|ascii|sixel]\n"
    "Options:\n"
    "  -h, --help             Print this help and exit\n"
    "  -o, --once             Run once and exit\n"
    "  -a, --all              Show --gpu --fan together (without --widgets)\n"
    "  -g, --gpu              Enable GPU monitoring (without --widgets)\n"
    "  -f, --fan              Enable motherboard fans + temps (without --widgets)\n"
    "  -S, --sunday-start     Calendar starts on Sunday\n"
    "  -w, --widgets <list>   Comma-separated widget list\n"
    "  -I, --ip-refresh <n>   Public IP refresh interval (default 86400)\n"
    "  -W, --weather-refresh <n>  Weather refresh interval (default 1800)\n";

void print_usage(void)
{
  sys_write(STDOUT_FILENO, HELP_STR, sizeof HELP_STR - 1);
}

static int eq_key(const char *p, const char *key, size_t klen)
{
  const char *e = strchr(p, '=');
  if (!e)
    return -1;
  if ((size_t)(e - p) != klen)
    return -1;
  if (memcmp(p, key, klen))
    return -1;
  return 0;
}

static int all_digits(const char *s)
{
  for (; *s; s++)
    if (*s < '0' || *s > '9')
      return 0;
  return 1;
}

int parse_uint(const char *s, int *field)
{
  char *end = NULL;
  errno = 0;
  unsigned long v = strtoul(s, &end, 10);
  if (*end || v > INT_MAX)
    return -1;
  *field = (int)v;
  return 0;
}

static int val_refresh(const char *p, const char *key, size_t klen, int *field)
{
  const char *e = strchr(p, '=');
  if (!e)
    return -1;
  if ((size_t)(e - p) != klen || memcmp(p, key, klen))
    return -1;
  if (!e[1] || !all_digits(e + 1))
    return -2;
  return parse_uint(e + 1, field);
}

static int val_widgets(struct args *a, const char *p)
{
  if (eq_key(p, "--widgets", sizeof "--widgets" - 1) < 0)
    return -1;
  const char *e = strchr(p, '=');
  if (!e[1]) {
    a->has_widgets = 1;
    a->widgets[0] = 0;
    return 0;
  }
  if (widget_validate(e + 1) != 0) {
    sys_write(STDERR_FILENO, "Invalid widget list\n", sizeof "Invalid widget list\n" - 1);
    return -3;
  }
  a->has_widgets = 1;
  strncpy(a->widgets, e + 1, sizeof a->widgets - 1);
  a->widgets[sizeof a->widgets - 1] = 0;
  return 0;
}

static int eq_val(struct args *a, const char *p)
{
  int r;
  r = val_refresh(p, "--ip-refresh", sizeof "--ip-refresh" - 1, &a->ip_refresh);
  if (r == 0)
    return 0;
  if (r == -2) {
    sys_write(STDERR_FILENO, "Invalid value for --ip-refresh\n", sizeof "Invalid value for --ip-refresh\n" - 1);
    return -3;
  }
  r = val_refresh(p, "--weather-refresh", sizeof "--weather-refresh" - 1, &a->weather_refresh);
  if (r == 0)
    return 0;
  if (r == -2) {
    sys_write(STDERR_FILENO, "Invalid value for --weather-refresh\n",
              sizeof "Invalid value for --weather-refresh\n" - 1);
    return -3;
  }
  return val_widgets(a, p);
}

static int pos_mode(struct args *a, const char *p)
{
  if (!strcmp(p, "text")) {
    a->mode = MODE_TEXT;
    return 0;
  }
  if (!strcmp(p, "ascii")) {
    a->mode = MODE_ASCII;
    return 0;
  }
  if (!strcmp(p, "sixel")) {
    a->mode = MODE_SIXEL;
    return 0;
  }
  if (!strcmp(p, "auto")) {
    a->mode = MODE_AUTO;
    return 0;
  }
  return -1;
}

static int b_once(struct args *a, const char *p)
{
  if (!strcmp(p, "--once")) {
    a->once = 1;
    return 0;
  }
  if (!strcmp(p, "-o")) {
    a->once = 1;
    return 0;
  }
  return -1;
}

static int b_all(struct args *a, const char *p)
{
  if (!strcmp(p, "--all")) {
    a->all = 1;
    return 0;
  }
  if (!strcmp(p, "-a")) {
    a->all = 1;
    return 0;
  }
  return -1;
}

static int b_gpu(struct args *a, const char *p)
{
  if (!strcmp(p, "--gpu")) {
    a->gpu = 1;
    return 0;
  }
  if (!strcmp(p, "-g")) {
    a->gpu = 1;
    return 0;
  }
  return -1;
}

static int b_fan(struct args *a, const char *p)
{
  if (!strcmp(p, "--fan")) {
    a->fan = 1;
    return 0;
  }
  if (!strcmp(p, "-f")) {
    a->fan = 1;
    return 0;
  }
  return -1;
}

static int b_sun(struct args *a, const char *p)
{
  if (!strcmp(p, "--sunday-start")) {
    a->sunday_start = 1;
    return 0;
  }
  if (!strcmp(p, "-S")) {
    a->sunday_start = 1;
    return 0;
  }
  return -1;
}

static int bool_flags(struct args *a, const char *p)
{
  if (b_once(a, p) == 0)
    return 0;
  if (b_all(a, p) == 0)
    return 0;
  if (b_gpu(a, p) == 0)
    return 0;
  if (b_fan(a, p) == 0)
    return 0;
  if (b_sun(a, p) == 0)
    return 0;
  return -1;
}

static int help_flag(const char *p)
{
  if (!strcmp(p, "--help"))
    return 1;
  if (!strcmp(p, "-h"))
    return 1;
  return 0;
}

static void unk(const char *a)
{
  int l = 0;
  sys_write(STDERR_FILENO, "Unknown argument: ", sizeof "Unknown argument: " - 1);
  while (a[l])
    l++;
  sys_write(STDERR_FILENO, a, l);
  sys_write(STDERR_FILENO, "\n", sizeof "\n" - 1);
}

static int parse_dash(struct args *a, const char *p, const char *n)
{
  int r = parse_short_group(a, p, n);
  if (r) {
    if (r < 0)
      return r;
    return r == 2 ? 1 : 0;
  }
  if (help_flag(p)) {
    print_usage();
    return -2;
  }
  if (!bool_flags(a, p))
    return 0;
  return val_flags(a, p, n);
}

static int parse_one(struct args *a, const char *p, const char *n)
{
  int r = eq_val(a, p);
  if (r == 0)
    return 0;
  if (r == -3)
    return -3;
  if (p[0] != '-')
    return pos_mode(a, p) ? -1 : 0;
  return parse_dash(a, p, n);
}

static void apply_all(struct args *a)
{
  if (!a->has_widgets && a->all) {
    a->gpu = 1;
    a->fan = 1;
  }
}

int parse_args(struct args *a, int argc, char **argv)
{
  memset(a, 0, sizeof *a);
  a->mode = MODE_AUTO;
  a->ip_refresh = DEFAULT_IP_REFRESH;
  a->weather_refresh = DEFAULT_WEATHER_REFRESH;
  for (int i = 1; i < argc; i++) {
    int t = parse_one(a, argv[i], i + 1 < argc ? argv[i + 1] : 0);
    switch (t) {
    case -1:
      unk(argv[i]);
      return 1;
    case -2:
      return -1;
    case -3:
      return 1;
    }
    i += t;
  }
  apply_all(a);
  return 0;
}
