#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <glob.h>               // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"
#include "fan_int.h"

static const char *HWMON_SKIP[] = {
  "k10temp", "coretemp", "nvme", "acpitz", "amdgpu",
  "nouveau", "thinkpad", "pmbus", NULL
};

static int is_skipped_hwmon(const char *name)
{
  for (const char **s = HWMON_SKIP; *s; s++)
    if (strcmp(name, *s) == 0)
      return 1;
  return 0;
}

static void strip_trail(char *buf)
{
  size_t len = strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    buf[--len] = 0;
}

static int is_mobo_hwmon(const char *dir)
{
  char path[PATH_SZ];
  snprintf(path, sizeof path, "%s/name", dir);
  char buf[CUP_BUF_SZ];
  if (read_file(path, buf, sizeof buf) != 0)
    return 0;
  strip_trail(buf);
  return !is_skipped_hwmon(buf);
}

static int fan_fb_ensure(struct cpu_keep *k, size_t need)
{
  if (need <= (size_t)k->fan_fb_cap)
    return 0;
  size_t new_cap = (need + 63) & ~63;
  char *n = realloc(k->fan_fb_valid, new_cap);
  if (!n)
    return -1;
  k->fan_fb_valid = n;
  k->fan_fb_cap = (int)new_cap;
  return 0;
}

static size_t fan_fb_write(struct cpu_keep *k, const glob_t *g)
{
  size_t off = 0;
  k->fan_fb_valid[0] = 0;
  for (size_t i = 0; i < g->gl_pathc; i++) {
    if (!is_mobo_hwmon(g->gl_pathv[i]))
      continue;
    const char *sep = off > 0 ? "," : "";
    int n = snprintf(k->fan_fb_valid + off, (size_t)(k->fan_fb_cap - off), "%s%s",
                     sep, g->gl_pathv[i]);
    if ((size_t)n >= (size_t)(k->fan_fb_cap - off))
      break;
    off += n;
  }
  return off;
}

static void fan_fb_rebuild(struct cpu_keep *k, const glob_t *g)
{
  size_t total = 1;
  for (size_t i = 0; i < g->gl_pathc; i++) {
    if (!is_mobo_hwmon(g->gl_pathv[i]))
      continue;
    total += strlen(g->gl_pathv[i]) + 1;
  }
  if (fan_fb_ensure(k, total) < 0)
    return;
  fan_fb_write(k, g);
}

static int fan_fb_read_one(struct fentry **all, int *all_n, int *all_cap, struct cpu_keep *k, int i)
{
  char p[PATH_SZ];
  snprintf(p, sizeof p, "%s/%s", k->fan_cache_dir, k->fan_ents[i].name);
  unsigned long long raw;
  if (!read_uint(p, &raw))
    copy_entry(&k->fan_ents[i], k->fan_ents[i].name, raw);
  if (ensure_cap(all, all_cap, *all_n + 1) < 0)
    return -1;
  (*all)[*all_n] = k->fan_ents[i];
  (*all_n)++;
  return 0;
}

static int fan_fb_read_dir(struct fentry **all, int *all_n, int *all_cap, struct cpu_keep *k, const char *dir)
{
  if (strcmp(dir, k->fan_cache_dir))
    fan_cache_init(k, dir);
  for (int i = 0; i < k->fan_n; i++)
    if (fan_fb_read_one(all, all_n, all_cap, k, i) < 0)
      return -1;
  return 0;
}

static void fan_fb_scan(char *l, int *np, struct cpu_keep *k)
{
  struct fentry *all = NULL;
  int all_n = 0, all_cap = 0;
  const char *cp = k->fan_fb_valid;
  while (*cp) {
    char dir[PATH_SZ];
    size_t len = strcspn(cp, ",");
    memcpy(dir, cp, len);
    dir[len] = 0;
    if (fan_fb_read_dir(&all, &all_n, &all_cap, k, dir) < 0)
      goto out;
    cp += len;
    if (*cp == ',')
      cp++;
  }
  if (all_n > 0)
    fmt_inputs(l, np, all, (size_t)all_n);
 out:
  free(all);
}

void fallback_scan(char *l, int *np, struct cpu_keep *k)
{
  if (!k->fan_fb_valid) {
    glob_t g;
    if (sys_glob("/sys/class/hwmon/hwmon*", 0, NULL, &g) != 0)
      return;
    fan_fb_rebuild(k, &g);
    sys_globfree(&g);
    if (!k->fan_fb_valid)
      return;
  }
  fan_fb_scan(l, np, k);
}
