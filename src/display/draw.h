#ifndef DRAW_H
#define DRAW_H

#define DOT_COLS 27
#define DOT_ROWS 5

struct display;

struct dots {
  char c[DOT_COLS][DOT_ROWS];
};

struct draw_ctx {
  int col;
  int tty;
};

void fill_dots(struct dots *d, int num, int pos);
void draw_col(char **pp, char u, char l);
void draw_digits(struct dots *d, int h, int m, int s);
int calc_hsize(int i, int size);
int render_line(char *buf, const struct dots *d, int i, int sz, const struct draw_ctx *dc);
void render_ascii(const struct draw_ctx *dc, int h, int m, int s, int size);
int render_wrapped(const char *panel, int info_w, int tty, const char *col_str, int max_lines);
void update_sidebar_lines(struct display *d, int lines, int once, int tty, const char *col_str);

#endif
