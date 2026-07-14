#define _GNU_SOURCE
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <glob.h>               // cppcheck-suppress missingIncludeSystem
#include "util/file_scan.h"
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

#define LOAD_INT_SCALE 65536.0
#define CPU_STAT_FIELDS 7
#define CPU_STAT_FIELD_MIN 4
#define CPU_IDLE_IDX 3
#define CPU_IOWAIT_IDX 4

static unsigned long long accumulate(const unsigned long long *v, int cnt)
{
  unsigned long long t = 0;
  for (int i = 0; i < cnt;)
    t += v[i++];
  return t;
}

static int match_aggregate(const char *p, size_t len)
{
  return len > 4 && memcmp(p, "cpu ", 4) == 0;
}

static int is_cpu_core(const char *p, size_t len)
{
  return len > 3 && memcmp(p, "cpu", 3) == 0 && p[3] >= '0' && p[3] <= '9';
}

static int parse_cpu_line(const char *b, unsigned long long *idle,
                          unsigned long long *total, unsigned long long *iowait)
{
  unsigned long long v[CPU_STAT_FIELDS];
  int cnt = sscanf(b, " %*s%llu%llu%llu%llu%llu%llu%llu",
                   v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6);
  if (cnt < CPU_STAT_FIELD_MIN)
    return -1;
  *iowait = v[CPU_IOWAIT_IDX], *idle = v[CPU_IDLE_IDX], *total = accumulate(v, cnt);
  return 0;
}

typedef struct {
  unsigned long long *idle, *total, *iowait;
  int *ok, *n;
} ChunkState;

static void process_line(const char *p, int len, ChunkState *st)
{
  if (!*st->ok && match_aggregate(p, len))
    *st->ok = parse_cpu_line(p, st->idle, st->total, st->iowait) == 0;
  if (is_cpu_core(p, len))
    (*st->n)++;
}

struct cpu_ctx {
  ChunkState *st;
};

static int cpu_line(void *ctx, const char *line, int len)
{
  struct cpu_ctx *cc = ctx;
  process_line(line, len, cc->st);
  return 0;
}

static int read_stat(unsigned long long *idle, unsigned long long *total, unsigned long long *iowait, int *ncpu)
{
  char buf[BIG_BUF];
  int ok = 0, n = 0;
  ChunkState st = { idle, total, iowait, &ok, &n };
  struct cpu_ctx cc = { &st };
  file_read_lines("/proc/stat", &cc, cpu_line, buf, sizeof buf);
  *ncpu = n;
  return ok ? 0 : -1;
}

static void store_prev(struct cpu_keep *k, unsigned long long idle, unsigned long long total,
                       unsigned long long iowait_v)
{
  k->cpu_prev_idle = idle;
  k->cpu_prev_total = total;
  k->cpu_prev_iowait = iowait_v;
}

int cpu_info_pct(struct clock_state *ci)
{
  unsigned long long idle = 0, total = 0, iowait_v = 0;
  if (read_stat(&idle, &total, &iowait_v, &ci->num_cpus) != 0)
    return 0;
  struct cpu_keep *k = &ci->keep;
  int r = 0;
  if (k->cpu_prev_total && total > k->cpu_prev_total) {
    unsigned long long d = total - k->cpu_prev_total;
    ci->iowait_pct = (int)((iowait_v - k->cpu_prev_iowait) * PERCENT_BASE / d);
    r = (int)((d - (idle - k->cpu_prev_idle)) * PERCENT_BASE / d);
  }
  store_prev(k, idle, total, iowait_v);
  return ci->pct = r;
}

static int cpu_temp_discover(struct cpu_keep *keep)
{
  glob_t g;
  if (sys_glob("/sys/bus/pci/drivers/k10temp/*/hwmon/hwmon*/temp1_input", 0, NULL, &g) != 0)
    if (sys_glob("/sys/devices/platform/coretemp.0/hwmon/hwmon*/temp1_input", 0, NULL, &g) != 0)
      return -1;
  snprintf(keep->cpu_temp_path, sizeof keep->cpu_temp_path, "%s", g.gl_pathv[0]);
  sys_globfree(&g);
  return 0;
}

int cpu_temp_c(const struct cpu_keep *keep)
{
  if (!keep->cpu_temp_path[0])
    return -1;
  return temp_from_milli(keep->cpu_temp_path);
}

static int freq_ensure_cap(struct cpu_keep *k, size_t need)
{
  if (need <= (size_t)k->cpu_freq_ndir_cap)
    return 0;
  unsigned new_cap = (need + 63) & ~63;
  char **n = realloc(k->cpu_freq_dirs, new_cap * sizeof *n);
  if (!n)
    return -1;
  k->cpu_freq_dirs = n;
  k->cpu_freq_ndir_cap = new_cap;
  return 0;
}

static void freq_discover_dirs(struct cpu_keep *k)
{
  glob_t g;
  char p[PATH_BUF_SZ];
  snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu*/cpufreq");
  if (sys_glob(p, 0, NULL, &g) != 0)
    return;
  if (freq_ensure_cap(k, g.gl_pathc) < 0) {
    sys_globfree(&g);
    return;
  }
  k->cpu_freq_ndir = 0;
  for (size_t i = 0; i < g.gl_pathc; i++)
    k->cpu_freq_dirs[k->cpu_freq_ndir++] = strdup(g.gl_pathv[i]);
  sys_globfree(&g);
}

void cpu_discover(struct cpu_keep *k)
{
  cpu_temp_discover(k);
  freq_discover_dirs(k);
}

static void add_cpu_freq(const char *dir, int *sum_cur, int *sum_max, int *n)
{
  char fp[PATH_SZ];
  unsigned long long v;
  snprintf(fp, sizeof fp, "%s/scaling_cur_freq", dir);
  if (read_uint(fp, &v) == 0) {
    *sum_cur += (int)(v / KHZ_PER_MHZ);
    (*n)++;
  }
  snprintf(fp, sizeof fp, "%s/scaling_max_freq", dir);
  if (read_uint(fp, &v) == 0)
    *sum_max += (int)(v / KHZ_PER_MHZ);
}

void get_cpu_freqs(struct clock_state *ci)
{
  const struct cpu_keep *k = &ci->keep;
  int sum_cur = 0, sum_max = 0, n = 0;
  for (unsigned i = 0; i < k->cpu_freq_ndir; i++)
    add_cpu_freq(k->cpu_freq_dirs[i], &sum_cur, &sum_max, &n);
  if (n > 0) {
    ci->freq = sum_cur / n;
    ci->freq_max = sum_max / n;
  } else {
    ci->freq = 0;
    ci->freq_max = 0;
  }
}

void get_cpu_extra(struct clock_state *ci)
{
  struct cpu_keep *k = &ci->keep;
  if (k->cpu_freq_ndir > 0) {
    if (read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", ci->governor, sizeof ci->governor) == 0) {
      size_t n = strlen(ci->governor);
      while (n > 0 && (ci->governor[n - 1] == '\n' || ci->governor[n - 1] == '\r'))
        ci->governor[--n] = 0;
    }
  }
  ci->load = (double)k->si.loads[0] / LOAD_INT_SCALE;
}
