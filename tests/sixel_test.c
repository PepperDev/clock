#include "mock_syscall.h"
#include "display/sixel.h"
#include "display/render.h"
#include "monitor/monitor.h"
#include "monitor/widget.h"
#include <string.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem -- musl include paths not known

static int test_da1_parse(void)
{
  static const char *c[] = {
    "\033[?4c", "\033[?2;4;6c", "\033[?2;6;4c",
    "\033[?2;6c", "\033[?42;4c", "\033[?42c",
    NULL, "", "xterm",
  };
  static const int e[] = { 1, 1, 1, 0, 1, 0, 0, 0, 0 };
  for (size_t i = 0; i < sizeof e / sizeof *e; i++)
    if (!!sixel_da1_parse(c[i]) != e[i])
      return (int)i + 1;
  return 0;
}

static int test_pixels_parse(void)
{
  int w, h;
  /* valid */
  if (sixel_pixels_parse("\033[4;595;960t", &w, &h) != 0 || w != 960 || h != 595)
    return 1;
  if (sixel_pixels_parse("\033[4;1080;1920t", &w, &h) != 0 || w != 1920 || h != 1080)
    return 2;
  /* zero pixels — valid parse but caller must reject zero */
  if (sixel_pixels_parse("\033[4;0;0t", &w, &h) != 0 || w != 0 || h != 0)
    return 3;
  /* trailing data */
  if (sixel_pixels_parse("prefix\033[4;300;400t", &w, &h) != 0 || w != 400 || h != 300)
    return 4;
  /* invalid */
  if (sixel_pixels_parse(NULL, &w, &h) != -1)
    return 5;
  if (sixel_pixels_parse("", &w, &h) != -1)
    return 6;
  if (sixel_pixels_parse("\033[4;;t", &w, &h) != -1)
    return 7;
  if (sixel_pixels_parse("\033[?4c", &w, &h) != -1)
    return 8;
  if (sixel_pixels_parse("garbage", &w, &h) != -1)
    return 9;
  return 0;
}

static struct widget_ctx wctx;

static void setup_ci(struct clock_state *ci)
{
  memset(ci, 0, sizeof *ci);
  ci->bat_pct = -1;
  ci->keep.widget = wctx;
  time_t t = 0;
  struct tm *gt = gmtime(&t);   // cppcheck-suppress constVariablePointer -- test helper
  if (gt)
    strftime(ci->date_str, sizeof ci->date_str, "%a %d %b %Y", gt);
}

static int check_sixel(const char *out)
{
  if (!out || !*out)
    return 1;
  if (!strstr(out, "\033Pq"))
    return 2;
  if (strstr(out, "\033\\"))
    return 0;
  size_t len = strlen(out);
  fprintf(stderr, "out len=%zu no ST, hex:", len);
  for (size_t i = 0; i < len; i++)
    fprintf(stderr, " %02x", (unsigned char)out[i]);
  fprintf(stderr, "\n");
  return 3;
}

static int test_render_sixel(void)
{
  widget_setup(&wctx, 0, NULL, 0, 0);
  struct display d;
  memset(&d, 0, sizeof d);
  d.wsrow = 24;
  d.wscol = 80;
  d.info_col = 54;
  d.cell_w = 10;
  d.cell_h = 18;
  struct tm tm;
  memset(&tm, 0, sizeof tm);
  tm.tm_hour = 12;
  tm.tm_min = 34;
  tm.tm_sec = 56;
  struct clock_state ci;
  setup_ci(&ci);
  mock_reset();
  render_sixel(&d, &tm, &ci, 0);
  return check_sixel(mock_get_output());
}

int main(void)
{
  widget_setup(&wctx, 0, NULL, 0, 0);
  int rc;
  rc = test_da1_parse();
  if (rc) {
    printf("FAIL test_da1_parse code=%d\n", rc);
    return rc;
  }
  rc = test_pixels_parse();
  if (rc) {
    printf("FAIL test_pixels_parse code=%d\n", rc);
    return rc;
  }
  rc = test_render_sixel();
  if (rc) {
    printf("FAIL test_render_sixel code=%d\n", rc);
    return rc;
  }
  printf("PASS\n");
  return 0;
}
