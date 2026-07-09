#define _GNU_SOURCE
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include "monitor_int.h"
#include "monitor.h"

struct bat_set {
  struct bat_sample *sa;
  int *idx;
  int *cnt;
  int dir;
};

static void bat_sel(struct bat_set *s, struct cpu_keep *k, int state)
{
  s->dir = state;
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
  k->bat_biased_next_chg = (state == 1);
  k->bat_biased_next_dchg = (state == 0);
  k->bat_unbiased_full_chg = 0;
  k->bat_unbiased_full_dchg = 0;
  return 1;
}

static void bat_prune_biased(struct cpu_keep *k, struct bat_set *s)
{
  if (s->dir)
    k->bat_unbiased_full_chg = 1;
  else
    k->bat_unbiased_full_dchg = 1;
  for (int i = 0; i < *s->cnt; i++)
    if (s->sa[i].biased)
      s->sa[i].duration_sec = 0;
}

static void bat_consolidate(struct cpu_keep *k, struct bat_set *s)
{
  int full = s->dir ? k->bat_unbiased_full_chg : k->bat_unbiased_full_dchg;
  if (full)
    return;
  int tot = 0;
  for (int i = 0; i < *s->cnt; i++)
    if (!s->sa[i].biased)
      tot += s->sa[i].duration_sec;
  if (tot >= 60)
    bat_prune_biased(k, s);
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
  bat_consolidate(k, &s);
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
    if (state == 0) {
      k->bat_idx_dchg = -1;
      k->bat_biased_next_dchg = 1;
    } else {
      k->bat_idx_chg = -1;
      k->bat_biased_next_chg = 1;
    }
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

static int bat_new_slot(struct cpu_keep *k, struct bat_set *s)
{
  int biased = s->dir ? k->bat_biased_next_chg : k->bat_biased_next_dchg;
  int full = s->dir ? k->bat_unbiased_full_chg : k->bat_unbiased_full_dchg;
  if (biased && full)
    return -1;
  slot_advance(s->idx, s->cnt);
  int si = *s->idx;
  s->sa[si].power_diff = 0;
  s->sa[si].biased = biased;
  s->sa[si].duration_sec = 0;
  if (s->dir)
    k->bat_biased_next_chg = 0;
  else
    k->bat_biased_next_dchg = 0;
  return si;
}

static int bat_ensure_slot(struct cpu_keep *k, struct bat_set *s, int delta)
{
  if (*s->idx < 0) {
    int si = bat_new_slot(k, s);
    if (si < 0)
      return -1;
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
    int si = bat_ensure_slot(k, &s, cur_raw - k->bat_last_raw);
    if (si >= 0) {
      int dur = (int)(now - k->bat_change_ts);
      s.sa[si].duration_sec = dur;
      if (dur >= 60) {
        bat_consolidate(k, &s);
        bat_rolloff(k, &s, now, cur_raw);
      }
    }
  }
  k->bat_last_raw = cur_raw;
}

void bat_estimate(struct clock_state *ci, int cur_raw, int state, time_t now)
{
  ci->bat_est_sec = -1;
  if (bat_init(ci, cur_raw, state, now))
    return;
  if (bat_tick(ci, state, now))
    return;
  bat_update(ci, cur_raw, state, now);
  bat_compute_est(ci, cur_raw, state);
}
