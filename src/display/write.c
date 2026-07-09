#include <unistd.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include "display/draw.h"
#include "display/esc.h"
#include "util/syscall.h"
#include "main.h"

enum { LINE_BUF = 4096, CLR_BUF = 1024 };

static int zw_vs16(const unsigned char *p, int len)
{
  if (len < 3)
    return 0;
  if (p[0] != 0xEF)
    return 0;
  if (p[1] != 0xB8)
    return 0;
  if ((p[2] & 0xF0) != 0x80)
    return 0;
  return 3;
}

static int zw_vssup(const unsigned char *p, int len)
{
  if (len < 4)
    return 0;
  if (p[0] != 0xF3)
    return 0;
  if (p[1] != 0xA0)
    return 0;
  if (p[2] != 0x84)
    return 0;
  if ((unsigned char)(p[3] - 0x80) > 0x2F)
    return 0;
  return 4;
}

static int zw_csi(const unsigned char *p, int len)
{
  if (len < 2)
    return 0;
  if (p[0] != 0x1B)
    return 0;
  if (p[1] != '[')
    return 0;
  for (int i = 2; i < len; i++)
    if ((unsigned char)(p[i] - 0x40) <= 0x3E)
      return i + 1;
  return 0;
}

static int is_zw(const char *p, const char *end)
{
  int len = (int)(end - p);
  int r = zw_vs16((const unsigned char *)p, len);
  if (r)
    return r;
  r = zw_vssup((const unsigned char *)p, len);
  if (r)
    return r;
  return zw_csi((const unsigned char *)p, len);
}

static int char_multi(unsigned char c, const char *p, const char *end, int *adv)
{
  int cl = 2 + (c >= 0xE0) + (c >= 0xF0);
  int vs = cl >= 3 && is_zw(p + cl, end) == 3;
  int w = 1 + (cl == 4) + (vs && cl == 3);
  *adv = vs ? cl + 3 : cl;
  return w;
}

static int char_step(const char *p, const char *end, int *adv)
{
  unsigned char c = (unsigned char)*p;
  if ((c & 0xC0) == 0x80) {
    *adv = 1;
    return 0;
  }
  int z = is_zw(p, end);
  if (z) {
    *adv = z;
    return 0;
  }
  if (c < 0x80) {
    *adv = 1;
    return 1;
  }
  return char_multi(c, p, end, adv);
}

static const char *advance_cols(const char *p, const char *end, int n)
{
  while (p < end && n > 0) {
    int adv;
    n -= char_step(p, end, &adv);
    p += adv;
  }
  return p;
}

static int col_width(const char *p, const char *end)
{
  int n = 0;
  while (p < end) {
    int adv;
    n += char_step(p, end, &adv);
    p += adv;
  }
  return n;
}

static void wl_fallback(const char *pos, int pos_n, const char *p, int clen, int pad)
{
  char sp[128];
  sys_write(STDOUT_FILENO, pos, pos_n);
  sys_write(STDOUT_FILENO, p, clen);
  while (pad > 0) {
    int n = pad < 128 ? pad : 128;
    memset(sp, ' ', (size_t)n);
    sys_write(STDOUT_FILENO, sp, (size_t)n);
    pad -= n;
  }
}

static int write_one_line(const char *pos, int pos_n, const char *p, int clen, int pad)
{
  char buf[LINE_BUF];
  int n = 0;
  memcpy(buf + n, pos, pos_n);
  n += pos_n;
  if (n + clen + pad > (int)sizeof buf) {
    wl_fallback(pos, pos_n, p, clen, pad);
    return 1;
  }
  memcpy(buf + n, p, clen);
  n += clen;
  memset(buf + n, ' ', pad);
  n += pad;
  sys_write(STDOUT_FILENO, buf, n);
  return 1;
}

struct wr_ctx {
  int info_w;
  int tty;
  const char *col_str;
  int max_lines;
  int count;
};

static int wr_limit(const struct wr_ctx *ctx)
{
  return ctx->max_lines > 0 && ctx->count >= ctx->max_lines;
}

static void wr_mkpos(const struct wr_ctx *ctx, char *pos, int *pos_n)
{
  *pos_n = 0;
  if (ctx->tty & TTY_CUP)
    *pos_n = snprintf(pos, CUP_BUF_SZ, ctx->tty & TTY_EL ? "%s" ESC_EL : "%s", ctx->col_str);
}

static void write_wrapped_lines(const char *start, const char *end, struct wr_ctx *ctx)
{
  char pos[CUP_BUF_SZ] = { 0 };
  int pos_n;
  wr_mkpos(ctx, pos, &pos_n);
  const char *p = start;
  while (p < end) {
    if (wr_limit(ctx))
      break;
    if (ctx->count > 0)
      sys_write(STDOUT_FILENO, "\n", 1);
    ctx->count++;
    const char *breakp = advance_cols(p, end, ctx->info_w);
    int pad = ctx->tty == TTY_CUP ? ctx->info_w - col_width(p, breakp) : 0;
    write_one_line(pos, pos_n, p, (int)(breakp - p), pad);
    p = breakp;
  }
}

static void write_wrapped(const char *start, const char *end, struct wr_ctx *ctx)
{
  if (end - start >= (int)(sizeof(ESC_EL) - 1))
    start += sizeof(ESC_EL) - 1;
  if (ctx->info_w < 1) {
    if (ctx->count > 0)
      sys_write(STDOUT_FILENO, "\n", 1);
    ctx->count++;
    sys_write(STDOUT_FILENO, start, end - start);
    return;
  }
  write_wrapped_lines(start, end, ctx);
}

int render_wrapped(const char *panel, int info_w, int tty, const char *col_str, int max_lines)
{
  struct wr_ctx ctx = {.info_w = info_w,.tty = tty,.col_str = col_str,.max_lines = max_lines,.count = 0 };
  const char *p = panel;
  while (*p) {
    if (wr_limit(&ctx))
      break;
    const char *nl = strchr(p, '\n');
    const char *end = nl ? nl : p + strlen(p);
    write_wrapped(p, end, &ctx);
    if (!nl)
      break;
    p = nl + 1;
  }
  return ctx.count;
}

static void clr_el(int diff)
{
  char buf[CLR_BUF] = { 0 };
  int n = 0;
  size_t eln = sizeof ESC_EL "\n" - 1;
  for (int i = 0; i < diff; i++) {
    if (n + (int)eln > (int)sizeof buf) {
      sys_write(STDOUT_FILENO, buf, n);
      n = 0;
    }
    memcpy(buf + n, ESC_EL "\n", eln);
    n += eln;
  }
  sys_write(STDOUT_FILENO, buf, n);
}

static void clr_spaces(const struct display *d, int diff, const char *col_str)
{
  int w = d->wscol - d->info_col;
  if (w < 1)
    return;
  char buf[CLR_BUF];
  int clen = (int)strlen(col_str);
  if (clen + w + 1 > (int)sizeof buf)
    return;
  memcpy(buf, col_str, clen);
  int n = clen;
  if (n + w + 1 > (int)sizeof buf)
    return;
  memset(buf + n, ' ', w);
  n += w;
  buf[n] = '\n';
  for (int i = 0; i < diff; i++)
    sys_write(STDOUT_FILENO, buf, n + 1);
}

void update_sidebar_lines(struct display *d, int lines, int once, int tty, const char *col_str)
{
  if (!once && lines < d->sidebar_lines) {
    int diff = d->sidebar_lines - lines;
    if (tty & TTY_EL)
      clr_el(diff);
    else if (tty & TTY_CUP)
      clr_spaces(d, diff, col_str);
  }
  d->sidebar_lines = lines;
}
