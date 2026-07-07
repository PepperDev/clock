#pragma once
#include <time.h>               // cppcheck-suppress missingIncludeSystem -- musl include paths not known
struct clock_state;
void set_cal_grid(struct clock_state *ci, const struct tm *t, int text, int ss, int tty);
