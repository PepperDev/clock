#define _GNU_SOURCE
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

void get_uptime(struct clock_state *ci)
{
  int s = (int)ci->keep.si.uptime;
  ci->uptime_d = s / SECS_PER_DAY, ci->uptime_h = (s % SECS_PER_DAY) / SECS_PER_HOUR, ci->uptime_m =
      (s % SECS_PER_HOUR) / SECS_PER_MIN;
}
