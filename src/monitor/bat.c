#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <glob.h>               // cppcheck-suppress missingIncludeSystem
#include <limits.h>             // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

#define BAT_NO_CHARGE_NOW (1u << 0)
#define BAT_NO_BATTERY    (1u << 1)
#define BAT_NO_ENERGY_NOW (1u << 2)
#define BAT_NO_FULL       (1u << 3)

static const char *battery_prefix(struct clock_state *ci)
{
  if (ci->keep.bat_flags & BAT_NO_BATTERY)
    return NULL;
  return ci->keep.bat_prefix[0] ? ci->keep.bat_prefix : NULL;
}

static int bat_read_field(const char *path)
{
  unsigned long long v;
  if (read_uint(path, &v))
    return -1;
  return v > (unsigned long long)INT_MAX ? INT_MAX : (int)v;
}

static int bat_read_raw(const struct cpu_keep *k, const char *pre)
{
  char p[PATH_BUF_SZ + 16];
  if (!(k->bat_flags & BAT_NO_CHARGE_NOW)) {
    snprintf(p, sizeof p, "%s/charge_now", pre);
    int v = bat_read_field(p);
    if (v >= 0)
      return v;
  }
  if (k->bat_flags & BAT_NO_ENERGY_NOW)
    return -1;
  snprintf(p, sizeof p, "%s/energy_now", pre);
  return bat_read_field(p);
}

static int bat_read_full(const char *pre)
{
  char p[PATH_BUF_SZ + 16];
  snprintf(p, sizeof p, "%s/charge_full", pre);
  int v = bat_read_field(p);
  if (v >= 0)
    return v;
  snprintf(p, sizeof p, "%s/energy_full", pre);
  return bat_read_field(p);
}

static int bat_status(const char *pre)
{
  char p[PATH_BUF_SZ + 16], b[RATE_SZ];
  snprintf(p, sizeof p, "%s/status", pre);
  if (read_file(p, b, sizeof b))
    return -1;
  if (b[0] == 'U')
    return -1;
  return (b[0] == 'C') + (b[0] == 'F' || b[0] == 'N') * 2;
}

static void bat_update_full(struct cpu_keep *k)
{
  int full = bat_read_full(k->bat_prefix);
  if (full >= 0)
    k->bat_charge_full_raw = full;
}

static int find_battery_prefix(struct cpu_keep *k)
{
  glob_t g;
  if (sys_glob("/sys/class/power_supply/*/type", 0, NULL, &g) != 0)
    return 1;
  for (size_t i = 0; i < g.gl_pathc; i++) {
    const char *pv = g.gl_pathv[i];
    char t[RATE_SZ];
    if (read_file(pv, t, sizeof t) || strncmp(t, "Battery", 7))
      continue;
    size_t l = strlen(pv) - 5;
    memcpy(k->bat_prefix, pv, l);
    k->bat_prefix[l] = 0;
    sys_globfree(&g);
    return 0;
  }
  sys_globfree(&g);
  return 1;
}

static void bat_try_energy_full(struct cpu_keep *k)
{
  char p[PATH_BUF_SZ + 16];
  snprintf(p, sizeof p, "%s/energy_full", k->bat_prefix);
  if (sys_access(p, F_OK) != 0) {
    k->bat_flags |= BAT_NO_FULL;
    return;
  }
  int full = bat_read_field(p);
  if (full >= 0)
    k->bat_charge_full_raw = full;
}

static void bat_discover_full(struct cpu_keep *k)
{
  char p[PATH_BUF_SZ + 16];
  snprintf(p, sizeof p, "%s/charge_now", k->bat_prefix);
  if (sys_access(p, F_OK) != 0)
    k->bat_flags |= BAT_NO_CHARGE_NOW;
  snprintf(p, sizeof p, "%s/energy_now", k->bat_prefix);
  if (sys_access(p, F_OK) != 0)
    k->bat_flags |= BAT_NO_ENERGY_NOW;
  snprintf(p, sizeof p, "%s/charge_full", k->bat_prefix);
  if (sys_access(p, F_OK) == 0) {
    bat_update_full(k);
  } else {
    bat_try_energy_full(k);
  }
}

void bat_discover(struct cpu_keep *k)
{
  if (find_battery_prefix(k))
    k->bat_flags |= BAT_NO_BATTERY;
  else
    bat_discover_full(k);
}

static int bat_ensure_full(struct cpu_keep *k)
{
  if (k->bat_flags & BAT_NO_FULL)
    return -1;
  if (k->bat_charge_full_raw != 0)
    return 0;
  int full = bat_read_full(k->bat_prefix);
  if (full < 0) {
    k->bat_flags |= BAT_NO_FULL;
    return -1;
  }
  k->bat_charge_full_raw = full;
  return 0;
}

static void bat_recalibrate_full(struct clock_state *ci, int cur_raw)
{
  if (ci->keep.bat_tstate != 2 || ci->keep.bat_prev_state == 2 || ci->bat_pct == PERCENT_BASE)
    return;
  bat_update_full(&ci->keep);
  if (ci->keep.bat_charge_full_raw <= 0)
    return;
  ci->bat_pct = (int)((long long)cur_raw * PERCENT_BASE / ci->keep.bat_charge_full_raw);
}

static void bat_process(struct clock_state *ci, const char *pre, int cur_raw)
{
  if (ci->keep.bat_charge_full_raw <= 0)
    return;
  ci->bat_pct = (int)((long long)cur_raw * PERCENT_BASE / ci->keep.bat_charge_full_raw);
  int st = bat_status(pre);
  if (st < 0) {
    ci->bat_pct = -1;
    return;
  }
  ci->bat_charging = st;
  bat_estimate(ci, cur_raw, st, ci->keep.realtime_ts.tv_sec);
  bat_recalibrate_full(ci, cur_raw);
}

void get_battery(struct clock_state *ci)
{
  ci->bat_pct = ci->bat_charging = ci->bat_est_sec = -1;
  const char *pre = battery_prefix(ci);
  if (!pre)
    return;
  int cur_raw = bat_read_raw(&ci->keep, pre);
  if (cur_raw < 0)
    cur_raw = ci->keep.bat_last_raw;
  if (cur_raw < 0)
    return;
  if (bat_ensure_full(&ci->keep))
    return;
  bat_process(ci, pre, cur_raw);
}
