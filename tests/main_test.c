#include "main.h"
#include "display/draw.h"
#include "display/esc.h"
#include "display/render.h"
#include "util/syscall.h"
#include "mock_syscall.h"
#include "monitor/monitor.h"
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <string.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known

static void render_panel(const struct clock_state *ci)
{
  char buf[1024];
  int n = render_panel_buf(buf, sizeof buf, ci);
  sys_write(STDOUT_FILENO, buf, n);
}

static int tparg1(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "--once", "text", NULL }) != 0 || !a.once || a.mode != MODE_TEXT || a.all)
    return 1;
  return 0;
}

static int tparg2(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "-o", "ascii", NULL }) != 0 || !a.once || a.mode != MODE_ASCII || a.all)
    return 2;
  return 0;
}

static int tparg3(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "--all", "-o", NULL }) != 0 || !a.all || !a.once || a.mode != MODE_AUTO)
    return 3;
  return 0;
}

static int tparg4(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "-a", NULL }) != 0 || !a.all || a.once || a.mode != MODE_AUTO)
    return 4;
  return 0;
}

static int tparg5(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "--help", NULL }) != -1)
    return 5;
  return 0;
}

static int tparg6(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "-h", NULL }) != -1)
    return 6;
  return 0;
}

static int tparg7(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "sixel", NULL }) != 0 || a.mode != MODE_SIXEL || a.once || a.all)
    return 7;
  return 0;
}

static int tparg8(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "--bogus", NULL }) != 1)
    return 8;
  return 0;
}

static int tparg9(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "invalid_mode", NULL }) != 1)
    return 9;
  return 0;
}

static int tparg10(void)
{
  struct args a;
  if (parse_args(&a, 1, (char *[]) { "clock", NULL }) != 0 || a.once || a.all || a.mode != MODE_AUTO)
    return 10;
  return 0;
}

static int tparg11(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "--gpu", NULL }) != 0 || !a.gpu || a.fan || a.mode != MODE_AUTO)
    return 11;
  return 0;
}

static int tparg12(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "-g", NULL }) != 0 || !a.gpu)
    return 12;
  return 0;
}

static int tparg13(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "--fan", NULL }) != 0 || !a.fan || a.gpu)
    return 13;
  return 0;
}

static int tparg14(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "-f", NULL }) != 0 || !a.fan)
    return 14;
  return 0;
}

static int tparg15(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "--sunday-start", NULL }) != 0 || !a.sunday_start)
    return 15;
  return 0;
}

static int tparg16(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "-S", NULL }) != 0 || !a.sunday_start)
    return 16;
  return 0;
}

static int tparg17(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "--widgets", "CPU,MEM", NULL }) != 0
      || !a.has_widgets || strcmp(a.widgets, "CPU,MEM") != 0)
    return 17;
  return 0;
}

static int tparg18(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "--widgets=NET,BAT", NULL }) != 0
      || !a.has_widgets || strcmp(a.widgets, "NET,BAT") != 0)
    return 18;
  return 0;
}

static int tparg19(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "-w", "", NULL }) != 0 || !a.has_widgets || a.widgets[0] != 0)
    return 19;
  return 0;
}

static int tparg20(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "--ip-refresh", "3600", NULL }) != 0 || a.ip_refresh != 3600)
    return 20;
  return 0;
}

static int tparg21(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "-I", "7200", NULL }) != 0 || a.ip_refresh != 7200)
    return 21;
  return 0;
}

static int tparg22(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "--ip-refresh=86400", NULL }) != 0 || a.ip_refresh != 86400)
    return 22;
  return 0;
}

static int tparg23(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "--ip-refresh", NULL }) == 0)
    return 23;
  return 0;
}

static int tparg24(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "--weather-refresh", "600", NULL }) != 0 || a.weather_refresh != 600)
    return 24;
  return 0;
}

static int tparg25(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "-W", "1200", NULL }) != 0 || a.weather_refresh != 1200)
    return 25;
  return 0;
}

static int tparg26(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "--weather-refresh", NULL }) == 0)
    return 26;
  return 0;
}

static int tparg27(void)
{
  struct args a;
  if (parse_args(&a, 2, (char *[]) { "clock", "auto", NULL }) != 0 || a.mode != MODE_AUTO)
    return 27;
  return 0;
}

static int tparg28(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "-ow=NET", "text", NULL }) != 0
      || !a.once || !a.has_widgets || strcmp(a.widgets, "NET") || a.mode != MODE_TEXT)
    return 28;
  return 0;
}

static int tparg29(void)
{
  struct args a;
  if (parse_args(&a, 4, (char *[]) { "clock", "-ow", "NET", "text", NULL }) != 0
      || !a.once || !a.has_widgets || strcmp(a.widgets, "NET") || a.mode != MODE_TEXT)
    return 29;
  return 0;
}

static int tparg30(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "-owNET", "text", NULL }) != 0
      || !a.once || !a.has_widgets || strcmp(a.widgets, "NET") || a.mode != MODE_TEXT)
    return 30;
  return 0;
}

static int tparg31(void)
{
  struct args a;
  if (parse_args(&a, 4, (char *[]) { "clock", "-ow", "", "text", NULL }) != 0
      || !a.once || !a.has_widgets || a.widgets[0] != 0 || a.mode != MODE_TEXT)
    return 31;
  return 0;
}

static int tparg32(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "-ow=", "text", NULL }) != 0
      || !a.once || !a.has_widgets || a.widgets[0] != 0 || a.mode != MODE_TEXT)
    return 32;
  return 0;
}

static int tparg33(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "-ow", "text", NULL }) != 0
      || !a.once || !a.has_widgets || a.widgets[0] != 0 || a.mode != MODE_TEXT)
    return 33;
  return 0;
}

static int tparg34(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "-ow", "-g", NULL }) != 0
      || !a.once || !a.has_widgets || a.widgets[0] != 0 || !a.gpu)
    return 34;
  return 0;
}

static int tparg35(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "-w", "xpto", NULL }) == 0)
    return 35;
  return 0;
}

static int tparg36(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "-w", "NET,xpto", NULL }) == 0)
    return 36;
  return 0;
}

static int tparg37(void)
{
  struct args a;
  if (parse_args(&a, 3, (char *[]) { "clock", "--widgets=", "text", NULL }) != 0
      || !a.has_widgets || a.widgets[0] != 0 || a.mode != MODE_TEXT)
    return 37;
  return 0;
}

static int tparg_g1(void)
{
  static int (*const f[])(void) = { tparg1, tparg2, tparg3, tparg4, tparg5 };
  for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
    int r = f[i] ();
    if (r)
      return r;
  }
  return 0;
}

static int tparg_g2(void)
{
  static int (*const f[])(void) = { tparg6, tparg7, tparg8, tparg9, tparg10 };
  for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
    int r = f[i] ();
    if (r)
      return r;
  }
  return 0;
}

static int tparg_g3(void)
{
  static int (*const f[])(void) = { tparg11, tparg12, tparg13, tparg14, tparg15 };
  for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
    int r = f[i] ();
    if (r)
      return r;
  }
  return 0;
}

static int tparg_g4(void)
{
  static int (*const f[])(void) = { tparg16, tparg17, tparg18, tparg19, tparg20 };
  for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
    int r = f[i] ();
    if (r)
      return r;
  }
  return 0;
}

static int tparg_g5(void)
{
  static int (*const f[])(void) = { tparg21, tparg22, tparg23, tparg24, tparg25 };
  for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
    int r = f[i] ();
    if (r)
      return r;
  }
  return 0;
}

static int tparg_g6(void)
{
  static int (*const f[])(void) = { tparg26, tparg27, tparg28, tparg29, tparg30 };
  for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
    int r = f[i] ();
    if (r)
      return r;
  }
  return 0;
}

static int tparg_g7(void)
{
  static int (*const f[])(void) = { tparg31, tparg32, tparg33, tparg34 };
  for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
    int r = f[i] ();
    if (r)
      return r;
  }
  return 0;
}

static int tparg_g8(void)
{
  static int (*const f[])(void) = { tparg35, tparg36, tparg37 };
  for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
    int r = f[i] ();
    if (r)
      return r;
  }
  return 0;
}

static int test_parse_args(void)
{
  static int (*const g[])(void) = { tparg_g1, tparg_g2, tparg_g3, tparg_g4, tparg_g5, tparg_g6, tparg_g7, tparg_g8 };
  for (size_t i = 0; i < sizeof g / sizeof *g; i++) {
    int r = g[i] ();
    if (r)
      return r;
  }
  return 0;
}

static int test_resolve_mode(void)
{
  if (resolve_mode(MODE_TEXT, 0) != MODE_TEXT)
    return 1;
  if (resolve_mode(MODE_TEXT, 1) != MODE_TEXT)
    return 2;
  if (resolve_mode(MODE_ASCII, 0) != MODE_ASCII)
    return 3;
  if (resolve_mode(MODE_AUTO, 0) != MODE_TEXT)
    return 4;
  if (resolve_mode(MODE_AUTO, 1) != MODE_AUTO)
    return 5;
  return 0;
}

static int test_recalc_size(void)
{
  unsigned short row, col;
  int size;
  recalc_size(&row, &col, &size, 24, 80 - 40);
  if (size < 1 || row > 24)
    return 1;
  recalc_size(&row, &col, &size, 10, 80 - 40);
  if (size < 1)
    return 2;
  recalc_size(&row, &col, &size, 100, 100 - 40);
  if (size < 1)
    return 3;
  return 0;
}

static int test_fill_dots(void)
{
  struct dots d;
  memset(&d, 0, sizeof d);
  fill_dots(&d, 8, 0);
  if (!d.c[0][0] || !d.c[0][4])
    return 1;
  fill_dots(&d, -1, 8);
  if (!d.c[8][1] || !d.c[8][3])
    return 2;
  return 0;
}

static int test_draw_col(void)
{
  char buf[10];
  char *p;
  p = buf;
  draw_col(&p, 1, 1);
  if (p - buf != 3)
    return 1;
  p = buf;
  draw_col(&p, 0, 0);
  if (p - buf != 1 || buf[0] != ' ')
    return 2;
  p = buf;
  draw_col(&p, 1, 0);
  if (p - buf != 3)
    return 3;
  p = buf;
  draw_col(&p, 0, 1);
  if (p - buf != 3)
    return 4;
  return 0;
}

static int test_draw_digits(void)
{
  struct dots d;
  memset(&d, 0, sizeof d);
  draw_digits(&d, 12, 34, 56);
  /* digit 1 at pos 0 uses right col (idx 2) → bottom row lit */
  if (!d.c[2][0])
    return 1;
  /* digit 5 at pos 20, left col (idx 20) → top row lit */
  if (!d.c[20][4])
    return 2;
  return 0;
}

static int test_calc_hsize(void)
{
  int h = calc_hsize(0, 2);
  if (h < 1)
    return 1;
  h = calc_hsize(0, 1);
  if (h < 1)
    return 2;
  return 0;
}

static int test_render_line_sz(int sz)
{
  struct dots d;
  memset(&d, 0, sizeof d);
  draw_digits(&d, 12, 0, 0);
  char buf[27 * 3 * 2 + 64];
  struct draw_ctx dc = {.col = 0,.tty = 1 };
  render_line(buf, &d, 0, sz, &dc);
  if (strlen(buf) == 0)
    return 1;
  return 0;
}

static int test_render_line(void)
{
  return test_render_line_sz(1);
}

static int test_render_line_size2(void)
{
  return test_render_line_sz(2);
}

static void mock_cpu_mem_common(const char *freq, const char *freq_max)
{
  mock_reset();
  mock_file("/proc/stat", "cpu  1000 0 0 0 0 0 0 0 0 0\n"
            "cpu0 200 100 50 50 0 0 0 0 0 0\n" "cpu1 200 100 50 50 0 0 0 0 0 0\n");
  mock_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", freq);
  mock_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", freq_max);
  mock_glob("/sys/devices/system/cpu/cpu*/cpufreq", (char *[]) { (char *)"/sys/devices/system/cpu/cpu0/cpufreq" }, 1);
  mock_file("/proc/meminfo",
            "MemTotal:      16384000 kB\n"
            "MemFree:        8192000 kB\n"
            "MemAvailable:   9216000 kB\n" "Buffers:        1000000 kB\n" "Cached:         5000000 kB\n");
  {
    struct sysinfo si = {.uptime = 123456 };
    mock_set_sysinfo(&si, 0);
  }
}

static int test_clock_main_once(void)
{
  mock_cpu_mem_common("2200000\n", "3700000\n");
  char *argv[] = { "clock", "--once", NULL };
  if (clock_main(2, argv) != 0)
    return 1;
  if (strlen(mock_get_output()) == 0)
    return 2;
  return 0;
}

static int test_clock_main_all(void)
{
  mock_cpu_mem_common("2200000\n", "3700000\n");
  char *argv[] = { "clock", "--once", "--all", NULL };
  if (clock_main(3, argv) != 0)
    return 1;
  if (strlen(mock_get_output()) == 0)
    return 2;
  return 0;
}

static int test_clock_main_once_ascii(void)
{
  mock_cpu_mem_common("2200000\n", "3700000\n");
  char *argv[] = { "clock", "--once", "ascii", NULL };
  if (clock_main(3, argv) != 0)
    return 1;
  if (strlen(mock_get_output()) == 0)
    return 2;
  return 0;
}

static int test_clock_main_mhz(void)
{
  mock_cpu_mem_common("800000\n", "1200000\n");
  if (clock_main(2, (char *[]) { "clock", "--once", NULL }) != 0)
    return 1;
  const char *out = mock_get_output();
  if (strlen(out) == 0)
    return 2;
  if (strstr(out, "MHz") == NULL)
    return 3;
  return 0;
}

static int test_clock_main_text(void)
{
  mock_cpu_mem_common("2200000\n", "3700000\n");
  if (clock_main(3, (char *[]) { "clock", "--once", "text", NULL }) != 0)
    return 1;
  const char *out = mock_get_output();
  if (strlen(out) == 0)
    return 2;
  if (strstr(out, "BAT ") != NULL)
    return 3;
  return 0;
}

static int test_clock_main_text_bat(void)
{
  mock_cpu_mem_common("2200000\n", "3700000\n");
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4250000\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Discharging\n");
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  if (clock_main(3, (char *[]) { "clock", "--once", "text", NULL }) != 0)
    return 1;
  const char *out = mock_get_output();
  if (strlen(out) == 0)
    return 2;
  if (strstr(out, "BAT 85%") == NULL)
    return 3;
  return 0;
}

static int test_clock_main_text_bat_unknown(void)
{
  mock_cpu_mem_common("2200000\n", "3700000\n");
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4250000\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Unknown\n");
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  if (clock_main(3, (char *[]) { "clock", "--once", "text", NULL }) != 0)
    return 1;
  if (strstr(mock_get_output(), "BAT") != NULL)
    return 2;
  return 0;
}

static int test_cleanup(void)
{
  mock_reset();
  struct clock_state c = { 0 };
  struct display d = {.cursor_hidden = 1,.is_tty = 1 };
  cleanup_all(&d, &c);
  const char *out = mock_get_output();
  if (strlen(out) == 0)
    return 1;
  return 0;
}

static int test_render_ascii(void)
{
  mock_reset();
  struct draw_ctx dc = {.col = 0,.tty = 1 };
  render_ascii(&dc, 12, 34, 56, 1);
  const char *out = mock_get_output();
  if (strlen(out) == 0)
    return 1;
  return 0;
}

static int test_position_cursor(void)
{
  mock_reset();
  position_cursor(5);
  const char *out = mock_get_output();
  if (strlen(out) == 0)
    return 1;
  return 0;
}

static int test_render_panel_fan(void)
{
  mock_reset();
  struct clock_state ci;
  memset(&ci, 0, sizeof ci);
  widget_setup(&ci.keep.widget, 1, "FAN", 0, 0);
  snprintf(ci.fan_line, sizeof ci.fan_line, "%s", "FAN 2200RPM 1500RPM\n38\xc2\xb0" "C 42\xc2\xb0" "C\n");
  render_panel(&ci);
  const char *out = mock_get_output();
  if (strstr(out, "2200RPM") == NULL)
    return 1;
  if (strstr(out, "38\xc2\xb0" "C") == NULL)
    return 2;
  return 0;
}

static int test_render_panel(void)
{
  mock_reset();
  struct clock_state ci;
  memset(&ci, 0, sizeof ci);
  widget_setup(&ci.keep.widget, 1, "DATE", 0, 0);
  snprintf(ci.date_str, sizeof ci.date_str, "%s", "Thu 12 Jun 2026");
  render_panel(&ci);
  const char *out = mock_get_output();
  if (strlen(out) == 0)
    return 1;
  if (strstr(out, "Thu 12 Jun 2026") == NULL)
    return 2;
  return 0;
}

static int test_render_panel_no_bat(void)
{
  mock_reset();
  struct clock_state ci;
  memset(&ci, 0, sizeof ci);
  widget_setup(&ci.keep.widget, 1, "BAT", 0, 0);
  ci.bat_pct = -1;
  render_panel(&ci);
  const char *out = mock_get_output();
  if (strstr(out, "BAT ") != NULL)
    return 1;
  return 0;
}

static int test_render_panel_bat(void)
{
  mock_reset();
  struct clock_state ci;
  memset(&ci, 0, sizeof ci);
  widget_setup(&ci.keep.widget, 1, "BAT", 0, 0);
  ci.bat_pct = 85;
  ci.bat_charging = 0;
  render_panel(&ci);
  const char *out = mock_get_output();
  if (strstr(out, "85%") == NULL)
    return 1;
  return 0;
}

static int test_render_wrapped(void)
{
  if (render_wrapped("line1\nline2\n", 0, 0, "") < 1)
    return 1;
  if (render_wrapped("abc def", 4, 1, "\033[6G") < 1)
    return 2;
  return 0;
}

static int test_wl_fallback(void)
{
  char *s = malloc(4101);
  if (!s)
    return 1;
  memset(s, 'A', 4100);
  s[4100] = 0;
  int lines = render_wrapped(s, 4100, TTY_CUP, "");
  free(s);
  if (lines < 1)
    return 2;
  return 0;
}

static int test_clock_main_invalid_widget(void)
{
  if (clock_main(5, (char *[]) { "clock", "-o", "-w", "xpto", "text", NULL }) != 1)
    return 1;
  return 0;
}

static int test_clock_main_invalid_grouped(void)
{
  char *a[] = { "clock", "-ow", "xpto", "text", NULL };
  if (clock_main(4, a) != 1)
    return 1;
  char *b[] = { "clock", "-ow=xpto", "text", NULL };
  if (clock_main(3, b) != 1)
    return 2;
  return 0;
}

static int test_clock_main_w_empty_text(void)
{
  mock_cpu_mem_common("2200000\n", "3700000\n");
  if (clock_main(4, (char *[]) { "clock", "-o", "-w", "text", NULL }) != 0)
    return 1;
  if (strlen(mock_get_output()) == 0)
    return 2;
  return 0;
}

int main(void)
{
  mock_syscall_real_threads = 1;
  static const struct {
    const char *name;
    int (*fn)(void);
    int off;
  } run[] = {
    {"test_parse_args", test_parse_args, 0},
    {"test_resolve_mode", test_resolve_mode, 200},

    {"test_recalc_size", test_recalc_size, 300},
    {"test_fill_dots", test_fill_dots, 400},
    {"test_draw_col", test_draw_col, 500},
    {"test_draw_digits", test_draw_digits, 600},
    {"test_calc_hsize", test_calc_hsize, 700},
    {"test_render_line", test_render_line, 800},
    {"test_clock_main_once", test_clock_main_once, 900},
    {"test_clock_main_all", test_clock_main_all, 950},
    {"test_cleanup", test_cleanup, 1000},
    {"test_render_ascii", test_render_ascii, 1100},
    {"test_position_cursor", test_position_cursor, 1200},
    {"test_render_panel", test_render_panel, 1300},
    {"test_render_panel_fan", test_render_panel_fan, 1310},
    {"test_render_panel_no_bat", test_render_panel_no_bat, 1320},
    {"test_render_panel_bat", test_render_panel_bat, 1330},
    {"test_clock_main_text", test_clock_main_text, 1400},
    {"test_clock_main_text_bat", test_clock_main_text_bat, 1410},
    {"test_clock_main_text_bat_unknown", test_clock_main_text_bat_unknown, 1415},
    {"test_render_line_size2", test_render_line_size2, 1450},
    {"test_render_wrapped", test_render_wrapped, 1460},
    {"test_wl_fallback", test_wl_fallback, 1470},
    {"test_clock_main_once_ascii", test_clock_main_once_ascii, 1500},
    {"test_clock_main_mhz", test_clock_main_mhz, 1550},
    {"test_clock_main_invalid_widget", test_clock_main_invalid_widget, 1560},
    {"test_clock_main_invalid_grouped", test_clock_main_invalid_grouped, 1570},
    {"test_clock_main_w_empty_text", test_clock_main_w_empty_text, 1580},
  };
  for (size_t i = 0; i < sizeof run / sizeof *run; i++) {
    printf("%s\n", run[i].name);
    int rc = run[i].fn();
    if (rc)
      return run[i].off + rc;
  }
  return 0;
}
