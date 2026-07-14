#define _GNU_SOURCE
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include "disk_impl.h"

struct fmt_ctx {
  const char *pfx, *name;
  int temp;
  unsigned long long rb, wb;
  unsigned long long size;
};

// cppcheck-suppress staticFunction - used by tests
int sto_fmt_thr(char *b, int z, unsigned long long v)
{
  if (v <= DISPLAY_UNIT_THRESHOLD)
    return snprintf(b, z, "%llub", v);
  if (v / BYTES_PER_KB_F <= DISPLAY_UNIT_THRESHOLD)
    return snprintf(b, z, "%.1fK", v / BYTES_PER_KB_F);
  return snprintf(b, z, "%.1fM", v / BYTES_PER_MB_F);
}

static int fmt_size(char *b, int z, unsigned long long bytes)
{
  unsigned long long mb = bytes / BYTES_PER_MB;
  if (!mb)
    return 0;
  if (mb <= DISPLAY_UNIT_THRESHOLD)
    return snprintf(b, z, "%lluM", mb);
  if (mb / BYTES_PER_KB_F <= DISPLAY_UNIT_THRESHOLD)
    return snprintf(b, z, "%.1fG", mb / BYTES_PER_KB_F);
  return snprintf(b, z, "%.1fT", mb / BYTES_PER_MB_F);
}

static int fmt_dev(char *b, int z, const struct fmt_ctx *fc)
{
  char rbuf[THR_STR_SZ], wbuf[THR_STR_SZ], sb[THR_STR_SZ] = "";
  sto_fmt_thr(rbuf, sizeof rbuf, fc->rb);
  sto_fmt_thr(wbuf, sizeof wbuf, fc->wb);
  if (fc->size)
    sb[0] = ' ', fmt_size(sb + 1, sizeof sb - 1, fc->size);
  if (fc->temp > 0)
    return snprintf(b, z, "%s %s %d\xc2\xb0" "C%s \xe2\x86\x93%s\xe2\x86\x91%s\n",
                    fc->pfx, fc->name, fc->temp, sb, rbuf, wbuf);
  return snprintf(b, z, "%s %s%s \xe2\x86\x93%s\xe2\x86\x91%s\n", fc->pfx, fc->name, sb, rbuf, wbuf);
}

static int line_avail(const struct clock_state *ci, const char *p)
{
  int rem = (int)(sizeof ci->sto_line) - (int)(p - ci->sto_line) - 1;
  return rem > 0 ? rem : 0;
}

static void safe_advance(char **pp, int cap, int n)
{
  if (n > 0 && cap > 0) {
    if (n >= cap)
      n = cap - 1;
    *pp += n;
  }
}

static const char *mount_ico(const struct clock_state *ci)
{
  return ci->keep.text ? "" : "\xf0\x9f\x93\x81";
}

static int mount_matches_dev(const struct clock_state *ci, unsigned idx, const struct mount *m)
{
  return !m->consumed && (!ci->keep.disk.devs[idx].major || mount_for_dev(&ci->keep.disk, idx, m->maj, m->min));
}

static void add_dev_mounts(char **pp, const struct clock_state *ci, unsigned idx, struct mount *mnts, int nm)
{
  const char *ico = mount_ico(ci);
  for (int j = 0; j < nm; j++) {
    if (!mount_matches_dev(ci, idx, &mnts[j]))
      continue;
    int cap = line_avail(ci, *pp);
    int nf = mount_fmt(*pp, cap, &mnts[j], ico);
    if (nf > 0) {
      safe_advance(pp, cap, nf);
      mnts[j].consumed = 1;
    }
  }
}

struct dev_info {
  const char *name, *icon;
  int temp;
  unsigned long long rs, ws;
  struct mount *mnts;
  int nm;
};

static void fmt_thr_dev(char **pp, struct clock_state *ci, unsigned idx, const struct dev_info *di)
{
  struct fmt_ctx fc = { di->icon, di->name, di->temp,
    sto_delta(di->rs, &ci->keep.disk.devs[idx].rp) * SECTOR_SIZE,
    sto_delta(di->ws, &ci->keep.disk.devs[idx].wp) * SECTOR_SIZE,
    ci->keep.disk.devs[idx].size
  };
  int cap = line_avail(ci, *pp);
  safe_advance(pp, cap, fmt_dev(*pp, cap, &fc));
  add_dev_mounts(pp, ci, idx, di->mnts, di->nm);
}

static void proc_one_dev(struct clock_state *ci, const char *path, struct mount *mnts, int nm, char **pp)
{
  unsigned idx;
  if (discover_dev(&ci->keep.disk, path, &idx))
    return;
  unsigned long long rs, ws;
  if (read_diskstat(path, &rs, &ws) != 0)
    return;
  int temp = dev_read_temp(&ci->keep.disk, idx, path);
  const char *icon = ci->keep.text ? "STO" : "\xf0\x9f\x97\x84\xef\xb8\x8f";
  struct dev_info di = { strrchr(path, '/') + 1, icon, temp, rs, ws, mnts, nm };
  fmt_thr_dev(pp, ci, idx, &di);
}

static void mark_dev_mounts(struct disk_ctx *d, unsigned idx, struct mount *mnts, int nm)
{
  unsigned major = d->devs[idx].major;
  for (int j = 0; j < nm; j++) {
    if (mnts[j].consumed)
      continue;
    if (major && !mount_for_dev(d, idx, mnts[j].maj, mnts[j].min))
      continue;
    mnts[j].consumed = 1;
  }
}

static void mark_consumed_mounts(struct disk_ctx *d, const glob_t *g, struct mount *mnts, int nm)
{
  for (size_t i = 0; i < g->gl_pathc; i++) {
    unsigned idx;
    if (discover_dev(d, g->gl_pathv[i], &idx))
      continue;
    mark_dev_mounts(d, idx, mnts, nm);
  }
}

static void output_unmatched_and_reset(const struct clock_state *ci, struct mount *mnts, int nm, char **pp)
{
  const char *ico = mount_ico(ci);
  for (int j = 0; j < nm; j++) {
    if (!mnts[j].consumed) {
      int cap = line_avail(ci, *pp);
      safe_advance(pp, cap, mount_fmt(*pp, cap, &mnts[j], ico));
    }
  }
  for (int j = 0; j < nm; j++)
    mnts[j].consumed = 0;
}

static void scan_block_devs(struct clock_state *ci, const glob_t *g, struct mount *mnts, int nm)
{
  mark_consumed_mounts(&ci->keep.disk, g, mnts, nm);
  char *p = ci->sto_line;
  output_unmatched_and_reset(ci, mnts, nm, &p);
  for (size_t i = 0; i < g->gl_pathc; i++)
    proc_one_dev(ci, g->gl_pathv[i], mnts, nm, &p);
}

static void evict_missing_devs(struct disk_ctx *d, const glob_t *g)
{
  unsigned w = 0;
  for (unsigned i = 0; i < d->cnt; i++) {
    int found = 0;
    for (size_t j = 0; j < g->gl_pathc; j++) {
      const char *name = strrchr(g->gl_pathv[j], '/') + 1;
      if (strcmp(d->devs[i].name, name) == 0) {
        found = 1;
        break;
      }
    }
    if (found) {
      if (w != i)
        d->devs[w] = d->devs[i];
      w++;
    }
  }
  d->cnt = w;
}

static void no_block_devs(struct clock_state *ci, struct mount_ctx *mc)
{
  const char *ico = mount_ico(ci);
  char *p = ci->sto_line;
  for (int j = 0; j < mc->nm; j++) {
    int cap = line_avail(ci, p);
    safe_advance(&p, cap, mount_fmt(p, cap, &mc->mnts[j], ico));
  }
}

void sto_read_throughput(struct clock_state *ci)
{
  ci->sto_line[0] = 0;
  struct mount_ctx *mc = ci->keep.disk.mnt;
  if (!mc) {
    mc = calloc(1, sizeof *mc);
    if (!mc)
      return;
    vfs_skip_init(mc);
    ci->keep.disk.mnt = mc;
  }
  mount_read(mc);
  glob_t g;
  if (sys_glob("/sys/block/*", 0, NULL, &g) != 0) {
    no_block_devs(ci, mc);
    return;
  }
  evict_missing_devs(&ci->keep.disk, &g);
  scan_block_devs(ci, &g, mc->mnts, mc->nm);
  sys_globfree(&g);
}
