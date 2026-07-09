#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <stddef.h>             // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include "main.h"
#include "argparse_int.h"
#include "util/syscall.h"
#include "monitor/widget.h"

#define REJ (unsigned short)-1

static const unsigned short SHORT_OFF[256] = {
  ['o'] = offsetof(struct args, once),
  ['S'] = offsetof(struct args, sunday_start),
  ['w'] = REJ,['I'] = REJ,['W'] = REJ,
};

static int is_number(const char *s)
{
  if (!s || !*s)
    return 0;
  for (; *s; s++)
    if (*s < '0' || *s > '9')
      return 0;
  return 1;
}

static int is_pos_mode(const char *s)
{
  return !strcmp(s, "auto") || !strcmp(s, "text") || !strcmp(s, "ascii") || !strcmp(s, "sixel");
}

static const char *wval_from_arg(const char *n)
{
  if (n && n[0] != '-' && !is_pos_mode(n))
    return n;
  return NULL;
}

static int v_widgets(struct args *a, const char *p, const char *n)
{
  if (!strcmp(p, "--widgets") || !strcmp(p, "-w")) {
    const char *val = wval_from_arg(n);
    a->has_widgets = 1;
    if (val) {
      if (widget_validate(val) != 0) {
        sys_write(STDERR_FILENO, "Invalid widget list\n", sizeof "Invalid widget list\n" - 1);
        return -3;
      }
      strncpy(a->widgets, val, sizeof a->widgets - 1);
      return 1;
    }
    a->widgets[0] = 0;
    return 0;
  }
  return -1;
}

static int print_flag_err(const char *msg, const char *p)
{
  int ml = 0;
  while (msg[ml])
    ml++;
  sys_write(STDERR_FILENO, msg, ml);
  int l = 0;
  while (p[l])
    l++;
  sys_write(STDERR_FILENO, p, l);
  sys_write(STDERR_FILENO, "\n", 1);
  return -3;
}

static int v_ip(struct args *a, const char *p, const char *n)
{
  if (strcmp(p, "--ip-refresh") && strcmp(p, "-I"))
    return -1;
  if (!n)
    return print_flag_err("Option requires a value: ", p);
  if (!is_number(n))
    return print_flag_err("Invalid value for ", p);
  return parse_uint(n, &a->ip_refresh), 1;
}

static int v_weather(struct args *a, const char *p, const char *n)
{
  if (strcmp(p, "--weather-refresh") && strcmp(p, "-W"))
    return -1;
  if (!n)
    return print_flag_err("Option requires a value: ", p);
  if (!is_number(n))
    return print_flag_err("Invalid value for ", p);
  return parse_uint(n, &a->weather_refresh), 1;
}

int val_flags(struct args *a, const char *p, const char *n)
{
  int t;
  if ((t = v_widgets(a, p, n)) != -1)
    return t;
  if ((t = v_ip(a, p, n)) != -1)
    return t;
  if ((t = v_weather(a, p, n)) != -1)
    return t;
  return -1;
}

static int is_short_group(const char *p)
{
  return p[0] == '-' && p[1] && p[2] && p[1] != '-';
}

static int apply_num(struct args *a, unsigned char ch, const char *val)
{
  if (!is_number(val))
    return -1;
  if (ch == 'I')
    return parse_uint(val, &a->ip_refresh);
  return parse_uint(val, &a->weather_refresh);
}

static int short_val_apply(struct args *a, unsigned char ch, const char *val)
{
  if (ch == 'w') {
    a->has_widgets = 1;
    if (widget_validate(val) != 0) {
      sys_write(STDERR_FILENO, "Invalid widget list\n", sizeof "Invalid widget list\n" - 1);
      return -3;
    }
    strncpy(a->widgets, val, sizeof a->widgets - 1);
    a->widgets[sizeof a->widgets - 1] = 0;
    return 0;
  }
  if (!val || !*val)
    return -1;
  return apply_num(a, ch, val);
}

static const char *find_short_val(const char *rest, const char *n)
{
  if (rest[0] == '=')
    return rest + 1;
  if (rest[0])
    return rest;
  if (n && n[0] != '-' && !is_pos_mode(n))
    return n;
  return NULL;
}

static int handle_rej_short(struct args *a, unsigned char ch, const char *rest, const char *n)
{
  const char *val = find_short_val(rest, n);
  if (!val) {
    if (ch == 'w') {
      a->has_widgets = 1;
      a->widgets[0] = 0;
      return 1;
    }
    char msg[] = "Option -? requires a numeric value\n";
    msg[8] = (char)ch;
    sys_write(STDERR_FILENO, msg, sizeof msg - 1);
    return -3;
  }
  int r = short_val_apply(a, ch, val);
  if (r != 0)
    return r;
  return val == n ? 2 : 1;
}

static int try_val_short(struct args *a, unsigned char ch, const char *rest, const char *n)
{
  if (ch == 'h') {
    print_usage();
    return -2;
  }
  unsigned short off = SHORT_OFF[ch];
  if (off == REJ)
    return handle_rej_short(a, ch, rest, n);
  if (!off)
    return -1;
  *(int *)((char *)a + off) = 1;
  return 0;
}

static int group_short(struct args *a, const char *p, const char *n)
{
  for (int i = 1; p[i]; i++) {
    int r = try_val_short(a, (unsigned char)p[i], p + i + 1, n);
    if (r) {
      if (r < 0)
        return r;
      return r;
    }
  }
  return 1;
}

int parse_short_group(struct args *a, const char *p, const char *n)
{
  if (!is_short_group(p))
    return 0;
  return group_short(a, p, n);
}
