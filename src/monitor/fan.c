#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <limits.h>             // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"
#include "fan_int.h"

enum { FAN_SZ = 256, FAN_RSV = 16 };

#define FAN_PFX "fan"
#define TEMP_PFX "temp"
#define FAN_PFX_LEN 3
#define TEMP_PFX_LEN 4

#define FAN_NO_MOBO (1u << 0)

static const char *MOBO_GLOBS[] = {
  "/sys/bus/wmi/drivers/dell_smm_hwmon/*/hwmon/hwmon*",
  "/sys/devices/platform/asus-nb-wmi/hwmon/hwmon*",
  "/sys/bus/platform/drivers/nct6687/*/hwmon/hwmon*",
  NULL
};

static int parent_exists(const char *path)
{
  size_t n = strcspn(path, "*");
  char buf[PATH_BUF_SZ];
  if (n == 0 || n >= sizeof buf)
    return 1;
  memcpy(buf, path, n);
  buf[n] = 0;
  char *slash = strrchr(buf, '/');
  if (slash)
    *slash = 0;
  return sys_access(buf, F_OK) == 0;
}

static void mobo_find(char *buf, size_t sz)
{
  glob_t g;
  for (const char **p = MOBO_GLOBS; *p; p++) {
    if (!parent_exists(*p))
      continue;
    if (sys_glob(*p, 0, NULL, &g) != 0)
      continue;
    if (g.gl_pathc > 0) {
      size_t n = strlen(g.gl_pathv[0]);
      if (n >= sz)
        n = sz - 1;
      memcpy(buf, g.gl_pathv[0], n);
      buf[n] = 0;
      sys_globfree(&g);
      return;
    }
    sys_globfree(&g);
  }
}

static int fmt_sensor(char *l, int np, const char *raw, int v, int first)
{
  if (np >= FAN_SZ - FAN_RSV)
    return np;
  if (strncmp(raw, FAN_PFX, FAN_PFX_LEN) == 0)
    return np + snprintf(l + np, (size_t)(FAN_SZ - FAN_RSV - np), first ? "FAN %dRPM" : " %dRPM", v);
  if (strncmp(raw, TEMP_PFX, TEMP_PFX_LEN) == 0)
    return np + snprintf(l + np, (size_t)(FAN_SZ - FAN_RSV - np), "%s%d\xc2\xb0" "C", first ? "" : " ", v);
  return np;
}

void copy_entry(struct fentry *e, const char *name, unsigned long long raw)
{
  size_t nl = strlen(name);
  if (nl >= sizeof e->name)
    nl = sizeof e->name - 1;
  memcpy(e->name, name, nl);
  e->name[nl] = 0;
  e->val = name[0] == 'f' ? (int)raw : (int)(raw / TEMP_MILLI_DIV);
}

static int is_fan_or_temp(const char *name)
{
  return strncmp(name, FAN_PFX, FAN_PFX_LEN) == 0 || strncmp(name, TEMP_PFX, TEMP_PFX_LEN) == 0;
}

int ensure_cap(struct fentry **ents, int *cap, int need)
{
  if (need <= *cap)
    return 0;
  if ((unsigned)need > UINT_MAX / sizeof **ents)
    return -1;
  struct fentry *ne = realloc(*ents, (unsigned)need * sizeof **ents);
  if (!ne)
    return -1;
  *ents = ne;
  *cap = need;
  return 0;
}

static int fill_entries(const glob_t *g, struct fentry **ents, int *cap)
{
  int n = 0;
  for (size_t i = 0; i < g->gl_pathc; i++) {
    const char *name = strrchr(g->gl_pathv[i], '/') + 1;
    if (!is_fan_or_temp(name))
      continue;
    if (ensure_cap(ents, cap, n + 1) < 0)
      return n;
    copy_entry(&(*ents)[n], name, 0);
    n++;
  }
  return n;
}

static int collect_inputs(const char *dir, struct fentry **ents, int *cap)
{
  char pat[PATH_SZ];
  snprintf(pat, sizeof pat, "%s/*_input", dir);
  glob_t g;
  if (sys_glob(pat, 0, NULL, &g))
    return 0;
  int n = fill_entries(&g, ents, cap);
  sys_globfree(&g);
  return n;
}

static int collect_and_format(char *l, int *np, const struct fentry *ents, size_t n, size_t *tidx)
{
  int ti = 0;
  int have_fan = 0;
  for (size_t i = 0; i < n; i++) {
    if (strncmp(ents[i].name, FAN_PFX, FAN_PFX_LEN) == 0) {
      *np = fmt_sensor(l, *np, ents[i].name, ents[i].val, !have_fan);
      have_fan = 1;
    }
    if (strncmp(ents[i].name, TEMP_PFX, TEMP_PFX_LEN) == 0)
      tidx[ti++] = i;
  }
  return ti;
}

static void fmt_temps_append(char *l, int *np, const struct fentry *ents, const size_t *tidx, int ti)
{
  for (int j = 0; j < ti; j++)
    *np = fmt_sensor(l, *np, ents[tidx[j]].name, ents[tidx[j]].val, !j);
  l[(*np)++] = '\n';
}

void fmt_inputs(char *l, int *np, const struct fentry *ents, size_t n)
{
  size_t *tidx = malloc(n * sizeof *tidx);
  int np0 = *np;
  int ti = collect_and_format(l, np, ents, n, tidx);
  if (*np > np0 && ti)
    l[(*np)++] = '\n';
  fmt_temps_append(l, np, ents, tidx, ti);
  free(tidx);
}

void fan_cache_init(struct cpu_keep *k, const char *dir)
{
  free(k->fan_ents);
  k->fan_ents = NULL;
  k->fan_cap = 0;
  size_t dl = strlen(dir);
  if (dl >= sizeof k->fan_cache_dir)
    dl = sizeof k->fan_cache_dir - 1;
  memcpy(k->fan_cache_dir, dir, dl);
  k->fan_cache_dir[dl] = 0;
  k->fan_n = collect_inputs(dir, &k->fan_ents, &k->fan_cap);
}

static void scan_hwmon_dir(char *l, int *np, const char *dir, struct cpu_keep *k)
{
  if (strcmp(dir, k->fan_cache_dir))
    fan_cache_init(k, dir);
  if (!k->fan_n)
    return;
  for (int i = 0; i < k->fan_n; i++) {
    char p[PATH_SZ];
    snprintf(p, sizeof p, "%s/%s", dir, k->fan_ents[i].name);
    unsigned long long raw;
    if (!read_uint(p, &raw))
      copy_entry(&k->fan_ents[i], k->fan_ents[i].name, raw);
  }
  fmt_inputs(l, np, k->fan_ents, (size_t)k->fan_n);
}

void fallback_scan(char *l, int *np, struct cpu_keep *k);

static void fan_line_term(char *l, int n)
{
  if (n > 0 && n < FAN_SZ)
    l[n] = 0;
}

void get_fan_info(struct clock_state *ci)
{
  char *l = ci->fan_line;
  *l = 0;
  struct cpu_keep *k = &ci->keep;
  char *hwmon = k->mobo_hwmon;
  if (!hwmon[0] && !(k->fan_flags & FAN_NO_MOBO)) {
    mobo_find(hwmon, sizeof k->mobo_hwmon);
    if (!hwmon[0])
      k->fan_flags |= FAN_NO_MOBO;
  }
  int n = 0;
  if (*hwmon)
    scan_hwmon_dir(l, &n, hwmon, k);
  else
    fallback_scan(l, &n, k);
  fan_line_term(l, n);
}
