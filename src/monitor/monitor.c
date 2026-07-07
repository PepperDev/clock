#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <fcntl.h>              // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <linux/netlink.h>      // cppcheck-suppress missingIncludeSystem
#include <arpa/inet.h>          // cppcheck-suppress missingIncludeSystem
#include <poll.h>               // cppcheck-suppress missingIncludeSystem
#include <errno.h>              // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include <stddef.h>             // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"
#include "widget.h"
#include "ioserv.h"

static int has_time_consumer(const struct widget_ctx *w)
{
  return widget_active(w, WIDGET_GPU) || widget_active(w, WIDGET_BAT)
      || widget_active(w, WIDGET_NET) || widget_active(w, WIDGET_WEATHER);
}

static void try_sys_sysinfo(struct clock_state *ci)
{
  if (widget_active(&ci->keep.widget, WIDGET_CPU) || widget_active(&ci->keep.widget, WIDGET_UP))
    sys_sysinfo(&ci->keep.si);
}

static void gather_cpu_freq(struct clock_state *ci)
{
  cpu_info_pct(ci);
  if (ci->num_cpus < 1)
    ci->num_cpus = 1;
  get_cpu_freqs(ci);
  ci->temp = cpu_temp_c(&ci->keep);
  get_cpu_extra(ci);
}

static void gather_cpu_mem_bat_up(struct clock_state *ci)
{
  try_sys_sysinfo(ci);
  if (widget_active(&ci->keep.widget, WIDGET_CPU))
    gather_cpu_freq(ci);
  if (widget_active(&ci->keep.widget, WIDGET_MEM))
    mem_usage(ci);
  if (widget_active(&ci->keep.widget, WIDGET_BAT))
    get_battery(ci);
  if (widget_active(&ci->keep.widget, WIDGET_UP))
    get_uptime(ci);
}

static void gather_net_sto_rest(struct clock_state *ci)
{
  if (widget_active(&ci->keep.widget, WIDGET_NET)) {
    get_net_info(ci);
    refresh_local_ips(ci);
  }
  if (widget_active(&ci->keep.widget, WIDGET_STO))
    sto_read_throughput(ci);
  if (widget_active(&ci->keep.widget, WIDGET_GPU))
    get_gpu_info(ci);
  if (widget_active(&ci->keep.widget, WIDGET_FAN))
    get_fan_info(ci);
}

static int any_data_widget(const struct widget_ctx *w)
{
  unsigned int m = (1u << WIDGET_CPU) | (1u << WIDGET_MEM) | (1u << WIDGET_BAT) | (1u << WIDGET_UP) |
      (1u << WIDGET_NET) | (1u << WIDGET_STO) | (1u << WIDGET_GPU) | (1u << WIDGET_FAN) | (1u << WIDGET_WEATHER);
  return w->active_mask & m;
}

void get_cpu_info(struct clock_state *ci, time_t now)
{
  if (!any_data_widget(&ci->keep.widget))
    return;
  unsigned m = ci->keep.widget.active_mask;
  if (m != ci->keep.active_mask) {
    memset(ci, 0, offsetof(struct clock_state, keep));
    ci->keep.active_mask = m;
  }
  ci->num_cpus = 1;
  if (has_time_consumer(&ci->keep.widget))
    ci->keep.realtime_ts.tv_sec = now;
  gather_cpu_mem_bat_up(ci);
  gather_net_sto_rest(ci);
  if (widget_active(&ci->keep.widget, WIDGET_MEM))
    get_container_mem(ci);
}
