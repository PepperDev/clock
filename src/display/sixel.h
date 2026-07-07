#ifndef SIXEL_H
#define SIXEL_H

#include "main.h"
#include "draw.h"
#include <time.h>               // cppcheck-suppress missingIncludeSystem

struct sixel_ctx {
  const struct dots *d;
  int iw, ih;
  int hm, vm;
};

struct sixel_size {
  int aw, ah;
  int dw, dh;
  int iw, ih;
  int cw, ch;
};

struct outer_rect {
  int aw, ah, dw, dh;
};

struct inner_dim {
  int iw, ih;
};

int sixel_detect(int is_tty_stdout, int is_tty_stdin);
int sixel_da1_parse(const char *buf);
int sixel_pixels_parse(const char *buf, int *wpix, int *hpix);
int sixel_query_pixels(int is_tty_stdout, int is_tty_stdin, int *wpix, int *hpix);
int px_val(const struct dots *d, int x, int row, int iw, int ih);
void encode_run(int val, int count);
void encode_band(int W, const int *vals);
struct sixel_size sixel_compute_size(int info_col, int wsrow, int is_tty, int cw, int ch);
void render_sixel(struct display *d, const struct tm *tm, const struct clock_state *ci, int once);

#endif
