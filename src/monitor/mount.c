#define _GNU_SOURCE
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <sys/statvfs.h>        // cppcheck-suppress missingIncludeSystem
#include <limits.h>             // cppcheck-suppress missingIncludeSystem
#include "util/file_scan.h"
#include "util/syscall.h"
#include "monitor_int.h"
#include "mount.h"

static const struct {
  const char *s;
  int len;
} VFS_SEED[] = {
  {"proc", 4}, {"sysfs", 5}, {"tmpfs", 5}, {"devtmpfs", 8}, {"cgroup", 6},
  {"cgroup2", 7}, {"debugfs", 7}, {"tracefs", 7}, {"pstore", 6},
  {"securityfs", 10}, {"hugetlbfs", 9}, {"configfs", 8}, {"efivarfs", 8},
  {"bpf", 3}, {"autofs", 6}, {"overlay", 7}, {"squashfs", 8}, {"devpts", 6},
  {"mqueue", 6}, {"fusectl", 7}, {"nsfs", 4}, {"ramfs", 5}, {"binfmt_misc", 11},
  {"nullfs", 7}, {"sockfs", 6}, {"pipefs", 7}, {"binder", 6}
};

static int vfs_has(const struct mount_ctx *mc, const char *fstype)
{
  for (int i = 0; i < mc->vfs_n; i++)
    if (strncmp(fstype, mc->vfs[i].name, mc->vfs[i].len) == 0)
      return 1;
  return 0;
}

static int vfs_grow(struct mount_ctx *mc)
{
  int cap = mc->vfs_cap;
  int step = cap > 65536 ? 65536 : cap > 64 ? cap : 64;
  int new_cap = cap + step;
  struct vfs_entry *p = realloc(mc->vfs, (unsigned)new_cap * sizeof(*p));
  if (!p)
    return -1;
  mc->vfs = p;
  mc->vfs_cap = new_cap;
  return 0;
}

static int vfs_add(struct mount_ctx *mc, const char *name, size_t len)
{
  if (mc->vfs_n >= mc->vfs_cap && vfs_grow(mc) != 0)
    return -1;
  char *dup = malloc(len + 1);
  if (!dup)
    return -1;
  memcpy(dup, name, len);
  dup[len] = 0;
  mc->vfs[mc->vfs_n].name = dup;
  mc->vfs[mc->vfs_n].len = len;
  mc->vfs_n++;
  return 0;
}

static int fs_line(void *ctx, const char *line, int len)
{
  struct mount_ctx *mc = ctx;
  if (len < 7 || memcmp(line, "nodev\t", 6) != 0)
    return 0;
  const char *name = line + 6;
  size_t nlen = (size_t)len - 6;
  if (!vfs_has(mc, name))
    return vfs_add(mc, name, nlen) < 0 ? 1 : 0;
  return 0;
}

void vfs_skip_init(struct mount_ctx *mc)
{
  mc->vfs = NULL;
  mc->vfs_n = 0;
  mc->vfs_cap = 0;
  for (size_t i = 0; i < sizeof VFS_SEED / sizeof *VFS_SEED; i++)
    vfs_add(mc, VFS_SEED[i].s, (size_t)VFS_SEED[i].len);
  char b[512];
  file_read_lines("/proc/filesystems", mc, fs_line, b, sizeof b);
  if (mc->vfs_n < mc->vfs_cap) {
    struct vfs_entry *p = realloc(mc->vfs, (unsigned)mc->vfs_n * sizeof(*p));
    if (p) {
      mc->vfs = p;
      mc->vfs_cap = mc->vfs_n;
    }
  }
}

void vfs_skip_free(struct mount_ctx *mc)
{
  for (int i = 0; i < mc->vfs_n; i++)
    free(mc->vfs[i].name);
  free(mc->vfs);
  mc->vfs = NULL;
  mc->vfs_n = 0;
  mc->vfs_cap = 0;
}

static void build_scan_fmt(char *fmt, size_t fmt_sz, size_t field_sz)
{
  snprintf(fmt, fmt_sz, "%%%zus", field_sz);
}

enum { FMT_SZ = 80, SCAN_SZ = 16 };

static int scan_fstype(const char *s, char *fstype, size_t sz)
{
  char fmt[SCAN_SZ];
  build_scan_fmt(fmt, sizeof fmt, sz - 1);
  return sscanf(s, fmt, fstype);
}

static int scan_mntpt(const char *line, struct mount *m, int *pos)
{
  char fmt[FMT_SZ], sf[SCAN_SZ];
  build_scan_fmt(sf, sizeof sf, sizeof m->mntpt - 1);
  snprintf(fmt, sizeof fmt, "%%u %%*u %%u:%%u %%*s %s%%n", sf);
  return sscanf(line, fmt, &m->mnt_id, &m->maj, &m->min, m->mntpt, pos);
}

static int parse_mnt(const char *line, struct mount *m, const struct mount_ctx *mc)
{
  const char *dash = strstr(line, " - ");
  if (!dash)
    return 0;
  char fstype[FSTYPE_SZ] = "";
  if (scan_fstype(dash + 3, fstype, sizeof fstype) != 1)
    return 0;
  if (vfs_has(mc, fstype))
    return 0;
  int pos = 0;
  if (scan_mntpt(line, m, &pos) < 4)
    return 0;
  m->consumed = 0;
  return m->maj != LOOP_MAJOR;
}

static int grow(struct mount_ctx *mc)
{
  int cap = mc->cap;
  int step = cap > 65536 ? 65536 : cap > 64 ? cap : 64;
  if (cap > INT_MAX - step - 63)
    return -1;
  int new = cap + step;
  new = (int)(((unsigned)new + 63) & ~63U);
  struct mount *p = realloc(mc->mnts, (unsigned)new * sizeof(*p));
  if (!p)
    return -1;
  mc->mnts = p;
  mc->cap = new;
  return 0;
}

static int shrink(struct mount_ctx *mc)
{
  if (mc->cap <= 64 || mc->nm > mc->cap / 4)
    return 0;
  struct mount *p = realloc(mc->mnts, (unsigned)mc->nm * sizeof(*p));
  if (!p)
    return 0;
  mc->mnts = p;
  mc->cap = mc->nm;
  return 0;
}

static struct mount *next_slot(struct mount_ctx *mc)
{
  if (mc->nm >= mc->cap && grow(mc) != 0)
    return NULL;
  return &mc->mnts[mc->nm];
}

int mount_for_dev(const struct disk_ctx *d, unsigned idx, unsigned maj, unsigned min)
{
  unsigned dm = d->devs[idx].minor;
  return maj == d->devs[idx].major && min >= dm && min < dm + 16;
}

static int fmt_mount(char *b, int z, const struct mount *m)
{
  unsigned long long total = m->total;
  unsigned long long used = total - m->free;
  int pc = total ? (int)(used * 100 / total) : 0;
  if (total <= DISPLAY_UNIT_THRESHOLD)
    return snprintf(b, z, " %s %d%% %llu/%lluM\n", m->mntpt, pc, used, total);
  if (total / BYTES_PER_KB_F <= DISPLAY_UNIT_THRESHOLD)
    return snprintf(b, z, " %s %d%% %.1f/%.1fG\n", m->mntpt, pc, (double)used / BYTES_PER_KB_F,
                    (double)total / BYTES_PER_KB_F);
  return snprintf(b, z, " %s %d%% %.1f/%.1fT\n", m->mntpt, pc, (double)used / BYTES_PER_MB_F,
                  (double)total / BYTES_PER_MB_F);
}

int mount_fmt(char *b, int z, const struct mount *m, const char *ico)
{
  int n = snprintf(b, z, "%s", ico);
  if (n >= z) {
    char tmp[1];
    return n + fmt_mount(tmp, 0, m);
  }
  return n + fmt_mount(b + n, z - n, m);
}

static int mnt_stat(const char *pt, struct mount *m)
{
  struct statvfs st;
  if (sys_statvfs(pt, &st) != 0 || st.f_blocks <= 0)
    return -1;
  m->total = (unsigned long long)st.f_blocks * st.f_frsize / BYTES_PER_MB;
  m->free = (unsigned long long)st.f_bfree * st.f_frsize / BYTES_PER_MB;
  return 0;
}

struct mnt_ctx {
  struct mount_ctx *mc;
};

static int match_or_add(struct mount_ctx *mc, const struct mount *tmp)
{
  for (int i = 0; i < mc->nm; i++)
    if (mc->mnts[i].mnt_id == tmp->mnt_id) {
      mc->mnts[i].consumed = 1;
      return 1;
    }
  struct mount *m = next_slot(mc);
  if (!m)
    return -1;
  *m = *tmp;
  m->consumed = 1;
  m->statvfs_tick = STATVFS_TICK_MAX;
  mc->nm++;
  mnt_stat(m->mntpt, m);
  return 0;
}

static int has_dup_majmin(const struct mount_ctx *mc, unsigned maj, unsigned min, unsigned mnt_id)
{
  for (int i = 0; i < mc->nm; i++)
    if (mc->mnts[i].maj == maj && mc->mnts[i].min == min && mc->mnts[i].mnt_id != mnt_id)
      return 1;
  return 0;
}

static int mnt_line(void *ctx, const char *line, int len)
{
  struct mnt_ctx *mc = ctx;
  struct mount tmp;
  if (len < 1 || !parse_mnt(line, &tmp, mc->mc))
    return 0;
  if (has_dup_majmin(mc->mc, tmp.maj, tmp.min, tmp.mnt_id))
    return 0;
  return match_or_add(mc->mc, &tmp) < 0 ? 1 : 0;
}

static void compact_removed(struct mount_ctx *mc)
{
  unsigned w = 0;
  for (int i = 0; i < mc->nm; i++)
    if (mc->mnts[i].consumed)
      mc->mnts[w++] = mc->mnts[i];
  mc->nm = (int)w;
  shrink(mc);
}

static void tick_statvfs_all(struct mount_ctx *mc)
{
  for (int i = 0; i < mc->nm; i++) {
    if (--mc->mnts[i].statvfs_tick <= 0) {
      mnt_stat(mc->mnts[i].mntpt, &mc->mnts[i]);
      mc->mnts[i].statvfs_tick = STATVFS_TICK_MAX;
    }
    mc->mnts[i].consumed = 0;
  }
}

int mount_read(struct mount_ctx *mc)
{
  for (int i = 0; i < mc->nm; i++)
    mc->mnts[i].consumed = 0;
  char b[1024];
  struct mnt_ctx mctx = { mc };
  if (file_read_lines("/proc/self/mountinfo", &mctx, mnt_line, b, sizeof b))
    return -1;
  compact_removed(mc);
  tick_statvfs_all(mc);
  return 0;
}
