#include <unistd.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include "display/draw.h"
#include "display/esc.h"
#include "util/syscall.h"
#include "main.h"

#define DISP_PAD 64
#define DIGIT_DATA_COLS 3
#define DIGIT_STEP (DIGIT_DATA_COLS + 1)
#define PAIR_STEP (2 * DIGIT_STEP + 2)

static const unsigned char DIGIT_DATA[10][DIGIT_DATA_COLS] = {
  {0x1F, 0x11, 0x1F}, {0x00, 0x00, 0x1F}, {0x17, 0x15, 0x1D},
  {0x15, 0x15, 0x1F}, {0x1C, 0x04, 0x1F}, {0x1D, 0x15, 0x17},
  {0x1F, 0x15, 0x17}, {0x10, 0x10, 0x1F}, {0x1F, 0x15, 0x1F},
  {0x1C, 0x14, 0x1F},
};

#define DIGIT_COUNT (int)(sizeof DIGIT_DATA / sizeof DIGIT_DATA[0])

// cppcheck-suppress staticFunction -- exposed for unit testing
void fill_dots(struct dots *d, int num, int pos)
{
  if (num >= 0 && num < DIGIT_COUNT) {
    for (unsigned col = 0; col < DIGIT_DATA_COLS; col++) {
      unsigned char bits = DIGIT_DATA[num][col];
      for (unsigned row = 0; row < DOT_ROWS; row++)
        d->c[pos + col][DOT_ROWS - 1 - row] = (bits >> row) & 1;
    }
  } else {
    d->c[pos][1] = 1;
    d->c[pos][3] = 1;
  }
}

// cppcheck-suppress staticFunction -- exposed for unit testing
void draw_col(char **pp, char u, char l)
{
  if (l && u) {
    *(*pp)++ = '\xe2';
    *(*pp)++ = '\x96';
    *(*pp)++ = '\x88';
  } else if (u) {
    *(*pp)++ = '\xe2';
    *(*pp)++ = '\x96';
    *(*pp)++ = '\x80';
  } else if (l) {
    *(*pp)++ = '\xe2';
    *(*pp)++ = '\x96';
    *(*pp)++ = '\x84';
  } else {
    *(*pp)++ = ' ';
  }
}

void draw_digits(struct dots *d, int h, int m, int s)
{
  fill_dots(d, h / 10, 0 * PAIR_STEP + 0 * DIGIT_STEP);
  fill_dots(d, h % 10, 0 * PAIR_STEP + 1 * DIGIT_STEP);
  fill_dots(d, -1, 2 * DIGIT_STEP);
  fill_dots(d, m / 10, 1 * PAIR_STEP + 0 * DIGIT_STEP);
  fill_dots(d, m % 10, 1 * PAIR_STEP + 1 * DIGIT_STEP);
  fill_dots(d, -1, 1 * PAIR_STEP + 2 * DIGIT_STEP);
  fill_dots(d, s / 10, 2 * PAIR_STEP + 0 * DIGIT_STEP);
  fill_dots(d, s % 10, 2 * PAIR_STEP + 1 * DIGIT_STEP);
}

static void repeat_col(char **pp, const struct dots *d, int j, int i, int sz)
{
  const char *save = *pp;
  int lline = (i * 2 + 1) / sz;
  draw_col(pp, d->c[j][i * 2 / sz], lline < DOT_ROWS ? d->c[j][lline] : 0);
  int len = *pp - save;
  for (int n = sz; --n; *pp += len)
    memcpy(*pp, save, len);
}

// cppcheck-suppress staticFunction -- exposed for unit testing
int render_line(char *buf, const struct dots *d, int i, int sz, const struct draw_ctx *dc)
{
  char *p = buf;
  if (dc->tty)
    p += snprintf(p, DOT_COLS * 3 * (size_t)sz + DISP_PAD, "\033[%dC", dc->col);
  for (unsigned j = 0; j < DOT_COLS; j++)
    repeat_col(&p, d, j, i, sz);
  *p = 0;
  return p - buf;
}

// cppcheck-suppress staticFunction -- exposed for unit testing
int calc_hsize(int i, int size)
{
  return i * 2 / size != (i * 2 + 3) / size ? 1 : (((i * 2) / size + 1) * size) / 2 - i;
}

void render_ascii(const struct draw_ctx *dc, int h, int m, int s, int size)
{
  struct dots d = { {{0}} };
  draw_digits(&d, h, m, s);
  int lines = (size * DOT_ROWS + 1) / 2;
  for (int i = 0; i < lines;) {
    char buf[DOT_COLS * 3 * size + DISP_PAD];
    int slen = render_line(buf, &d, i, size, dc);
    int hsize = calc_hsize(i, size);
    for (int n = hsize; n--;) {
      buf[slen] = '\n';
      sys_write(STDOUT_FILENO, buf, slen + 1);
    }
    i += hsize;
  }
}
