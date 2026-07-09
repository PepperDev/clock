#ifndef MAIN_H
#define MAIN_H

#include <signal.h>             // cppcheck-suppress missingIncludeSystem
#include <termios.h>            // cppcheck-suppress missingIncludeSystem
#include "display/draw.h"
#include "monitor/monitor_int.h"

enum mode { MODE_AUTO, MODE_TEXT, MODE_ASCII, MODE_SIXEL };

#define DEFAULT_IP_REFRESH 86400
#define DEFAULT_WEATHER_REFRESH 1800
#define NS_PER_SEC 1000000000

struct args {
  enum mode mode;
  int once;
  int sunday_start;
  int has_widgets;
  char widgets[PATH_SZ];
  int ip_refresh;
  int weather_refresh;
};

struct display {
  unsigned short wsrow, wscol;
  unsigned short row, col;
  int size, info_col;
  int sunday_start;
  int is_tty;
  int is_tty_stdin;
  int cell_w, cell_h;
  int sidebar_lines;
  int cursor_hidden;
  int sixel_mode;
  int termios_valid;
  volatile sig_atomic_t resized;
  struct termios saved_termios;
};

#include "display/layout.h"

struct clock_state;

int parse_args(struct args *a, int argc, char **argv);
enum mode resolve_mode(enum mode m, int tty);
int clock_main(int argc, char **argv);
void cleanup_all(struct display *d, struct clock_state *c);
void setup_if_tty(enum mode *m, int once, struct display *d);
void setup_sixel_cleanup(enum mode m, struct display *d);
enum mode setup_tty_and_mode(const struct args *a, struct clock_state *c, struct display *d, unsigned long long t0);
extern __thread volatile sig_atomic_t tls_terminated;
extern __thread struct display *tls_display;

#endif
