#define _GNU_SOURCE
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <limits.h>             // cppcheck-suppress missingIncludeSystem
#include "disk_impl.h"

static int check_virt_symlink(const char *path)
{
  char vp[PATH_SZ];
  const char *name = strrchr(path, '/') + 1;
  snprintf(vp, sizeof vp, "/sys/devices/virtual/block/%s", name);
  if (sys_access(vp, F_OK) == 0)
    return 1;
  static const char *TBL[] = { "ram", "loop", "dm-", "zram", "md" };
  for (size_t i = 0; i < sizeof TBL / sizeof *TBL; i++)
    if (strncmp(name, TBL[i], strlen(TBL[i])) == 0)
      return 1;
  return 0;
}

unsigned long long sto_delta(unsigned long long cur, unsigned long long *prev)
{
  unsigned long long p = *prev;
  return *prev = cur, p == (unsigned long long)-1 ? 0 : cur - p;
}

static int disk_grow(struct disk_ctx *d)
{
  unsigned cap = d->cap;
  unsigned step = cap > 65536 ? 65536 : cap > 64 ? cap : 64;
  if (cap > UINT_MAX - step - 63)
    return -1;
  unsigned ncap = cap + step;
  ncap = (ncap + 63) & ~63U;
  struct dev_out *nd = realloc(d->devs, ncap * sizeof *nd);
  if (!nd)
    return -1;
  d->devs = nd;
  d->cap = ncap;
  return 0;
}

static void disk_dev_init(struct dev_out *de, const char *name, unsigned maj, unsigned min, int virt)
{
  strncpy(de->name, name, sizeof de->name - 1);
  de->name[sizeof de->name - 1] = 0;
  de->rp = de->wp = (unsigned long long)-1;
  de->major = maj;
  de->minor = min;
  de->is_virtual = virt;
  de->has_temp = 0;
  de->temp_path[0] = 0;
  de->size = 0;
}

int read_diskstat(const char *path, unsigned long long *rs, unsigned long long *ws)
{
  char sp[PATH_BUF_SZ];
  snprintf(sp, sizeof sp, "%s/stat", path);
  char *buf = slurp(sp);
  if (!buf)
    return -1;
  int r = sscanf(buf, "%*u %*u %llu %*u %*u %*u %llu", rs, ws);
  free(buf);
  return r < 2 ? -1 : 0;
}

static void disk_find_temp_path(struct disk_ctx *d, const char *p, unsigned i)
{
  char tp[PATH_SZ];
  glob_t tg;
  snprintf(tp, sizeof tp, "%s/device/hwmon*/temp*_input", p);
  if (sys_glob(tp, 0, NULL, &tg) == 0 && tg.gl_pathc) {
    snprintf((char *)d->devs[i].temp_path, sizeof d->devs[i].temp_path, "%s", tg.gl_pathv[0]);
    d->devs[i].has_temp = 1;
  }
  sys_globfree(&tg);
}

static int read_hwmon_temp(struct disk_ctx *d, const char *p, unsigned idx)
{
  if (d->devs[idx].has_temp == 0) {
    disk_find_temp_path(d, p, idx);
    if (!d->devs[idx].temp_path[0])
      d->devs[idx].has_temp = -1;
  }
  if (!d->devs[idx].temp_path[0])
    return -1;
  return temp_from_milli((const char *)d->devs[idx].temp_path);
}

static void read_dev_size(struct disk_ctx *d, unsigned idx, const char *path)
{
  char p[DESC_LEN];
  snprintf(p, sizeof p, "%s/size", path);
  unsigned long long sectors;
  if (read_uint(p, &sectors) == 0)
    d->devs[idx].size = sectors * SECTOR_SIZE;
}

static void read_dev_majmin(const char *path, unsigned *maj, unsigned *min)
{
  char p[DESC_LEN], buf[PATH_BUF_SZ];
  snprintf(p, sizeof p, "%s/dev", path);
  if (read_file(p, buf, sizeof buf) == 0)
    sscanf(buf, "%u:%u", maj, min);
}

static unsigned discover_add(struct disk_ctx *d, const char *name, const char *path)
{
  int virt = check_virt_symlink(path);
  unsigned maj = 0, min = 0;
  read_dev_majmin(path, &maj, &min);
  if (d->cnt >= d->cap && disk_grow(d) < 0)
    return d->cnt;
  unsigned i = d->cnt++;
  disk_dev_init(&d->devs[i], name, maj, min, virt);
  if (!d->devs[i].size && !virt)
    read_dev_size(d, i, path);
  return i;
}

int discover_dev(struct disk_ctx *d, const char *path, unsigned *idx)
{
  const char *name = strrchr(path, '/') + 1;
  for (unsigned i = 0; i < d->cnt; i++)
    if (strcmp(d->devs[i].name, name) == 0) {
      *idx = i;
      return d->devs[i].is_virtual;
    }
  *idx = discover_add(d, name, path);
  return d->devs[*idx].is_virtual;
}

int dev_read_temp(struct clock_state *ci, struct disk_ctx *d, unsigned idx, const char *path)
{
  int t = -1;
  if (d->devs[idx].major)
    t = read_hwmon_temp(d, path, idx);
  if (t > ci->sto_temp)
    ci->sto_temp = t;
  return t;
}
