#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <glob.h>               // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include <limits.h>             // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

#define BAT_NO_CHARGE_NOW (1u << 0)
#define BAT_NO_BATTERY    (1u << 1)
#define BAT_NO_ENERGY_NOW (1u << 2)
#define BAT_NO_FULL       (1u << 3)

static int find_battery_prefix(struct clock_state *ci)
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
    memcpy(ci->keep.bat_prefix, pv, l);
    ci->keep.bat_prefix[l] = 0;
    break;
  }
  sys_globfree(&g);
  return !ci->keep.bat_prefix[0];
}

static const char *battery_prefix(struct clock_state *ci)
{
  if (ci->keep.bat_flags & BAT_NO_BATTERY)
    return NULL;
  if (!ci->keep.bat_prefix[0] && find_battery_prefix(ci)) {
    ci->keep.bat_flags |= BAT_NO_BATTERY;
    return NULL;
  }
  return ci->keep.bat_prefix;
}

static int slot_advance(int *idx, int *count)
{
  if (*count < BAT_NUM_SAMPLES) {
    *idx = *count;
    (*count)++;
  } else {
    *idx = (*idx + 1) % BAT_NUM_SAMPLES;
  }
  return *idx;
}

struct bat_set {
  struct bat_sample *sa;
  int *idx;
  int *cnt;
};

static void bat_sel(struct bat_set *s, struct cpu_keep *k, int state)
{
  if (state == 0) {
    s->sa = k->bat_samples_dchg;
    s->idx = &k->bat_idx_dchg;
    s->cnt = &k->bat_discharge_count;
  } else {
    s->sa = k->bat_samples_chg;
    s->idx = &k->bat_idx_chg;
    s->cnt = &k->bat_charge_count;
  }
}

static void bat_sum(struct bat_sample *a, int n, long long *tp, int *tt)
{
  *tp = 0;
  *tt = 0;
  for (int i = 0; i < n; i++) {
    if (a[i].duration_sec > 0) {
      long long d = a[i].power_diff;
      *tp += d > 0 ? d : -d;
      *tt += a[i].duration_sec;
    }
  }
}

static void bat_compute_est(struct clock_state *ci, int cur_raw, int state)
{
  struct cpu_keep *k = &ci->keep;
  struct bat_set s;
  bat_sel(&s, k, state);
  if (*s.cnt > 0) {
    long long tp;
    int tt;
    bat_sum(s.sa, *s.cnt, &tp, &tt);
    if (tp == 0)
      return;
    long long rem = state ? (long long)(k->bat_charge_full_raw - cur_raw)
        : (long long)cur_raw;
    ci->bat_est_sec = (int)(rem * tt / tp);
  }
}

static int bat_init(struct clock_state *ci, int cur_raw, int state, time_t now)
{
  struct cpu_keep *k = &ci->keep;
  if (k->bat_inited)
    return 0;
  k->bat_inited = 1;
  k->bat_tstate = state;
  k->bat_prev_state = state;
  k->bat_last_raw = cur_raw;
  k->bat_change_ts = now;
  k->bat_idx_chg = -1;
  k->bat_idx_dchg = -1;
  return 1;
}

static void bat_finalize_slot(struct cpu_keep *k, int prev_state, time_t now)
{
  if (prev_state != 0 && prev_state != 1)
    return;
  struct bat_set s;
  bat_sel(&s, k, prev_state);
  if (*s.idx < 0)
    return;
  s.sa[*s.idx].duration_sec = (int)(now - k->bat_change_ts);
}

static int bat_tick(struct clock_state *ci, int state, time_t now)
{
  struct cpu_keep *k = &ci->keep;
  k->bat_prev_state = k->bat_tstate;
  k->bat_tstate = state;

  if (k->bat_prev_state != state)
    bat_finalize_slot(k, k->bat_prev_state, now);

  if (state == 2)
    return 1;
  if (k->bat_prev_state != state) {
    if (state == 0)
      k->bat_idx_dchg = -1;
    else
      k->bat_idx_chg = -1;
    k->bat_change_ts = now;
  }
  return 0;
}

static void bat_rolloff(struct cpu_keep *k, struct bat_set *s, time_t now, int cur_raw)
{
  *s->idx = -1;
  k->bat_change_ts = now;
  k->bat_last_raw = cur_raw;
}

static int bat_ensure_slot(struct bat_set *s, int delta)
{
  if (*s->idx < 0) {
    slot_advance(s->idx, s->cnt);
    s->sa[*s->idx].power_diff = 0;
  }
  int si = *s->idx;
  s->sa[si].power_diff += delta;
  return si;
}

static void bat_update(struct clock_state *ci, int cur_raw, int state, time_t now)
{
  struct cpu_keep *k = &ci->keep;
  if (k->bat_last_raw != cur_raw) {
    struct bat_set s;
    bat_sel(&s, k, state);
    int si = bat_ensure_slot(&s, cur_raw - k->bat_last_raw);
    int dur = (int)(now - k->bat_change_ts);
    s.sa[si].duration_sec = dur;
    if (dur >= 60)
      bat_rolloff(k, &s, now, cur_raw);
  }
  k->bat_last_raw = cur_raw;
}

static void bat_estimate(struct clock_state *ci, int cur_raw, int state, time_t now)
{
  ci->bat_est_sec = -1;
  if (bat_init(ci, cur_raw, state, now))
    return;
  if (bat_tick(ci, state, now))
    return;
  bat_update(ci, cur_raw, state, now);
  bat_compute_est(ci, cur_raw, state);
}

static int bat_read_field(const char *path)
{
  unsigned long long v;
  if (read_uint(path, &v))
    return -1;
  return v > (unsigned long long)INT_MAX ? INT_MAX : (int)v;
}

static int bat_read_raw(struct cpu_keep *k, const char *pre)
{
  char p[PATH_BUF_SZ + 16];
  unsigned guard = (unsigned)(-!k->bat_charge_full_raw);
  if (!(k->bat_flags & BAT_NO_CHARGE_NOW)) {
    snprintf(p, sizeof p, "%s/charge_now", pre);
    int v = bat_read_field(p);
    if (v >= 0)
      return v;
    k->bat_flags |= guard & BAT_NO_CHARGE_NOW;
  }
  if (k->bat_flags & BAT_NO_ENERGY_NOW)
    return -1;
  snprintf(p, sizeof p, "%s/energy_now", pre);
  int v = bat_read_field(p);
  if (v >= 0)
    return v;
  k->bat_flags |= guard & BAT_NO_ENERGY_NOW;
  return -1;
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

static int bat_ensure_full(struct cpu_keep *k, const char *pre)
{
  if (k->bat_flags & BAT_NO_FULL)
    return -1;
  if (k->bat_charge_full_raw != 0)
    return 0;
  int full = bat_read_full(pre);
  if (full < 0) {
    k->bat_flags |= BAT_NO_FULL;
    return -1;
  }
  k->bat_charge_full_raw = full;
  return 0;
}

static void bat_recalibrate_full(struct clock_state *ci, const char *pre, int cur_raw, int fresh)
{
  if (ci->keep.bat_tstate != 2 || ci->keep.bat_prev_state == 2 || ci->bat_pct == PERCENT_BASE)
    return;
  int full = fresh ? ci->keep.bat_charge_full_raw : bat_read_full(pre);
  if (full <= 0)
    return;
  ci->keep.bat_charge_full_raw = full;
  ci->bat_pct = (int)((long long)cur_raw * PERCENT_BASE / full);
}

static void bat_process(struct clock_state *ci, const char *pre, int cur_raw, int fresh)
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
  bat_recalibrate_full(ci, pre, cur_raw, fresh);
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
  int fresh = (ci->keep.bat_charge_full_raw == 0);
  if (bat_ensure_full(&ci->keep, pre))
    return;
  bat_process(ci, pre, cur_raw, fresh);
}
