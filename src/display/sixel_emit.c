#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include "display/sixel.h"
#include "display/render.h"
#include "display/draw.h"
#include "display/esc.h"
#include "util/syscall.h"
#include "monitor/monitor.h"

enum { HDR_SZ = 64, OVERLAY_BUF = 1024, CUP_BUF = 16, SIXEL_BAND_H = 6 };
#define ALL_SIXEL_BG 63

static void sixel_emit_header(int W, int H)
{
  char hdr[HDR_SZ];
  int n = snprintf(hdr, sizeof hdr, "\033Pq\"1;1;%d;%d", W, H);
  sys_write(STDOUT_FILENO, hdr, n);
  sys_write(STDOUT_FILENO, "#0;2;90;90;90#1;2;13;13;13", sizeof "#0;2;90;90;90#1;2;13;13;13" - 1);
}

static int band_val(const struct sixel_ctx *ctx, int x, int rs)
{
  int v = 0;
  for (int i = 0; i < 6; i++) {
    int pr = rs + i;
    if (pr >= 0 && pr < ctx->ih && px_val(ctx->d, x, pr, ctx->iw, ctx->ih))
      v |= 1 << i;
  }
  return v;
}

static void sixel_write_band(int W, int bg_val, int has_fg, const int *vals)
{
  sys_write(STDOUT_FILENO, "#0", sizeof "#0" - 1);
  encode_run(bg_val, W);
  if (has_fg)
    sys_write(STDOUT_FILENO, "#1$", sizeof "#1$" - 1), encode_band(W, vals);
}

static int sixel_band_bg_val(int b, int bands, int H)
{
  return (b + 1 >= bands) ? (1 << (H % SIXEL_BAND_H + SIXEL_BAND_H * !(H % SIXEL_BAND_H))) - 1 : ALL_SIXEL_BG;
}

static int sixel_fill_band(const struct sixel_ctx *ctx, int W, int rs, int *out)
{
  int hf = 0;
  for (int x = 0; x < W; x++)
    hf |= out[x] = band_val(ctx, x - ctx->hm, rs);
  return hf;
}

static void sixel_emit_bands(const struct sixel_ctx *ctx, int W, int H)
{
  int *vals = malloc((size_t)W * sizeof *vals);
  if (!vals)
    return;
  int bands = (H + SIXEL_BAND_H - 1) / SIXEL_BAND_H;
  for (int b = 0; b < bands; b++) {
    int rs = b * SIXEL_BAND_H - ctx->vm;
    int has_fg = sixel_fill_band(ctx, W, rs, vals);
    sixel_write_band(W, sixel_band_bg_val(b, bands, H), has_fg, vals);
    if (b + 1 < bands)
      sys_write(STDOUT_FILENO, "-", 1);
  }
  free(vals);
}

static void overlay_non_tty(const struct clock_state *ci)
{
  char pbuf[OVERLAY_BUF];
  int n = fmt_widget_list(pbuf, sizeof pbuf - 1, ci, "");
  pbuf[n] = '\n';
  sys_write(STDOUT_FILENO, pbuf, n + 1);
}

static void sixel_emit_overlay(struct display *d, const struct clock_state *ci, int once, int dh)
{
  if (!d->is_tty) {
    overlay_non_tty(ci);
    return;
  }
  emit_widget_panel(d, ci, once, dh);
}

static void sixel_cup(int row, int col)
{
  char buf[CUP_BUF];
  int n = snprintf(buf, sizeof buf, "\033[%d;%dH", row, col);
  sys_write(STDOUT_FILENO, buf, n);
}

static void sixel_ahead(const struct display *d, int dw, int dh)
{
  if (!d->is_tty)
    return;
  int r = (d->wsrow - dh) / 2 + 1;
  int c = (d->info_col - dw) / 2 + 1;
  if (r < 1)
    r = 1;
  if (c < 1)
    c = 1;
  sixel_cup(r, c);
}

static void sixel_st(void)
{
  sys_write(STDOUT_FILENO, ESC_ST, sizeof ESC_ST - 1);
}

static void sixel_draw_digits(struct dots *dt, const struct tm *tm)
{
  memset(dt, 0, sizeof *dt);
  draw_digits(dt, tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static void sixel_emit_image(const struct dots *dt, const struct sixel_size *sz)
{
  int hm = (sz->dw * sz->cw - sz->iw) / 2;
  int pw = sz->dw * sz->cw;
  int ph = sz->dh * sz->ch;
  int ih = sz->ih;
  if ((ph - ih) & 1)
    ih += 1;
  int vm = (ph - ih) / 2;
  struct sixel_ctx ctx = {.d = dt,.iw = sz->iw,.ih = ih,.hm = hm,.vm = vm };
  sixel_emit_header(pw, ph);
  sixel_emit_bands(&ctx, pw, ph);
  sixel_st();
}

static void sixel_cuf(const struct display *d, int dw)
{
  if (!d->is_tty)
    return;
  int n = (d->info_col - dw) / 2;
  if (n < 1)
    return;
  char buf[CUP_BUF];
  int m = snprintf(buf, sizeof buf, "\033[%dC", n);
  sys_write(STDOUT_FILENO, buf, m);
}

void render_sixel(struct display *d, const struct tm *tm, const struct clock_state *ci, int once)
{
  struct dots dt;
  sixel_draw_digits(&dt, tm);
  struct sixel_size sz = sixel_compute_size(d->info_col, d->wsrow, d->is_tty, d->cell_w, d->cell_h);
  if (once)
    sixel_cuf(d, sz.dw);
  else
    sixel_ahead(d, sz.dw, sz.dh);
  sixel_emit_image(&dt, &sz);
  sixel_emit_overlay(d, ci, once, sz.dh);
}
