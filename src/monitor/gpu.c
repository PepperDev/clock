#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include <glob.h>               // cppcheck-suppress missingIncludeSystem
#include <fcntl.h>              // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

typedef unsigned long long ull;
static int fmt_pp(char *buf, size_t sz, const char *a, const char *b)
{
  int n = snprintf(buf, sz, "%s%s", a, b);
  if (n <= 0 || (size_t)n >= sz)
    return -1;
  return 0;
}

static void find_card(struct gpu_ctx *gpu)
{
  glob_t g;
  if (sys_glob("/sys/class/drm/card[0-9]*", 0, NULL, &g) != 0)
    return;
  int best_num = -1;
  for (size_t i = 0; i < g.gl_pathc; i++) {
    int n;
    if (sscanf(strrchr(g.gl_pathv[i], '/') + 1, "card%d", &n) != 1)
      continue;
    if (best_num < 0 || n < best_num) {
      best_num = n;
      snprintf(gpu->card_path, sizeof gpu->card_path, "%s", g.gl_pathv[i]);
    }
  }
  snprintf(gpu->card_token, sizeof gpu->card_token, "%d", best_num);
  sys_globfree(&g);
}

static int gpu_has_file(const struct gpu_ctx *gpu, const char *suffix)
{
  char p[PATH_BUF_SZ];
  if (fmt_pp(p, sizeof p, gpu->card_path, suffix) < 0)
    return 0;

  return sys_access(p, F_OK) == 0;
}

static void gpu_detect_type(struct gpu_ctx *gpu)
{
  if (gpu_has_file(gpu, "/device/gpu_busy_percent"))
    gpu->present |= GPU_HAS_BUSY_PCT;
  else if (gpu_has_file(gpu, "/gt_act_freq_mhz") && gpu_has_file(gpu, "/gt_max_freq_mhz"))
    gpu->present |= GPU_HAS_GT_FREQ;
}

static void gpu_find_hwmon(struct gpu_ctx *gpu)
{
  char p[PATH_BUF_SZ];
  if (fmt_pp(p, sizeof p, gpu->card_path, "/device/hwmon/hwmon*") < 0)
    return;
  glob_t gl;
  if (sys_glob(p, 0, NULL, &gl) != 0)
    return;
  if (gl.gl_pathc > 0)
    snprintf(gpu->hwmon_path, sizeof gpu->hwmon_path, "%s", gl.gl_pathv[0]);
  sys_globfree(&gl);
}

struct probe_entry {
  const char *suffix;
  unsigned int flag;
};

static const struct probe_entry PROBE_TBL[] = {
  {"/device/mem_info_vis_vram_total", GPU_HAS_MEM},
  {"/device/power_dpm_force_performance_level", GPU_HAS_GOV},
  {"/power/rc6_residency_ms", GPU_HAS_RC6},
};

static unsigned int gpu_probe_hwmon(const struct gpu_ctx *gpu)
{
  if (!gpu->hwmon_path[0])
    return 0;
  unsigned int p = 0;
  char path[PATH_SZ];
  snprintf(path, sizeof path, "%s/temp1_input", gpu->hwmon_path);
  if (sys_access(path, F_OK) == 0)
    p |= GPU_HAS_TEMP;
  snprintf(path, sizeof path, "%s/fan1_input", gpu->hwmon_path);
  if (sys_access(path, F_OK) == 0)
    p |= GPU_HAS_FAN;
  return p;
}

static unsigned int gpu_probe_features(const struct gpu_ctx *gpu)
{
  unsigned int p = 0;
  if (!(gpu->present & GPU_HAS_GT_FREQ)) {
    if (gpu_has_file(gpu, "/device/pp_dpm_sclk"))
      p |= GPU_HAS_PP_SCLK;
    if (gpu_has_file(gpu, "/device/pp_dpm_mclk"))
      p |= GPU_HAS_PP_MCLK;
  }
  for (size_t i = 0; i < sizeof PROBE_TBL / sizeof PROBE_TBL[0]; i++)
    if (gpu_has_file(gpu, PROBE_TBL[i].suffix))
      p |= PROBE_TBL[i].flag;
  return p | gpu_probe_hwmon(gpu);
}

static int read_gpu_uint(const struct gpu_ctx *gpu, const char *suffix, ull *v)
{
  char p[PATH_BUF_SZ];
  if (fmt_pp(p, sizeof p, gpu->card_path, suffix) < 0)
    return -1;
  return read_uint(p, v);
}

static ull gpu_read_temp(const struct gpu_ctx *gpu)
{
  if (!(gpu->present & GPU_HAS_TEMP))
    return (ull) - 1;
  int t = temp_from_milli(gpu->temp_path);
  return t < 0 ? (ull) - 1 : (ull) t;
}

static int gpu_read_mem(struct clock_state *ci)
{
  if (!(ci->keep.gpu.present & GPU_HAS_MEM))
    return -1;
  if (read_gpu_uint(&ci->keep.gpu, "/device/mem_info_vis_vram_total", &ci->gpu_mem_total) != 0)
    return -1;
  if (!ci->gpu_mem_total)
    return -1;
  return read_gpu_uint(&ci->keep.gpu, "/device/mem_info_vis_vram_used", &ci->gpu_mem_used);
}

static int read_gpu_file(const struct gpu_ctx *gpu, const char *suffix, char *buf, size_t sz)
{
  char p[PATH_BUF_SZ];
  if (fmt_pp(p, sizeof p, gpu->card_path, suffix) < 0)
    return -1;
  int fd = sys_open(p, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t r = sys_read(fd, buf, sz - 1);
  sys_close(fd);
  if (r <= 0)
    return -1;
  buf[r] = 0;
  return 0;
}

static ull parse_after_colon(const char *buf, const char *pos)
{
  while (pos > buf && *pos != ':')
    pos--;
  if (*pos == ':')
    return strtoull(pos + 1, NULL, 10);
  return 0;
}

static const char *last_line(const char *buf)
{
  const char *last = buf;
  for (const char *p = buf; *p; p++)
    if (*p == '\n' && p[1])
      last = p + 1;
  return last;
}

static void read_amd_freq_pair(const struct gpu_ctx *gpu, const char *suffix, ull *freq, ull *freq_max)
{
  char p[PATH_BUF_SZ];
  if (fmt_pp(p, sizeof p, gpu->card_path, suffix) < 0)
    return;
  char *buf = slurp(p);
  if (!buf)
    return;
  const char *m = strchr(buf, '*');
  if (m)
    *freq = parse_after_colon(buf, m);
  const char *sep = strchr(last_line(buf), ':');
  if (sep)
    *freq_max = strtoull(sep + 1, NULL, 10);
  free(buf);
}

static ull dms_ts(time_t s, long ns, ull ps, ull pns)
{
  int b = (long)ns < (long)pns;
  return ((ull) s - (ull) ps - (ull) b) * 1000ULL + (ull) ((long)ns - (long)pns + b * 1000000000L) / 1000000ULL;
}

static int gpu_rc6_pct(struct gpu_ctx *gpu, ull *pct, const struct timespec *ts)
{
  if (!(gpu->present & GPU_HAS_RC6))
    return -1;
  ull rc6;
  if (read_gpu_uint(gpu, "/power/rc6_residency_ms", &rc6) != 0)
    return -1;
  if (gpu->rc6_prev) {
    ull dms = dms_ts(ts->tv_sec, ts->tv_nsec, gpu->ts_sec, gpu->ts_nsec);
    if (dms && dms > rc6 - gpu->rc6_prev)
      *pct = PERCENT_BASE - (rc6 - gpu->rc6_prev) * PERCENT_BASE / dms;
  }
  gpu->rc6_prev = rc6;
  gpu->ts_sec = ts->tv_sec;
  gpu->ts_nsec = ts->tv_nsec;
  return 0;
}

static void gpu_read_intel(struct gpu_ctx *gpu, ull *pct, ull *freq, ull *freq_max, const struct timespec *ts)
{
  if (!(gpu->present & GPU_HAS_GT_FREQ))
    return;
  ull cur;
  if (read_gpu_uint(gpu, "/gt_act_freq_mhz", &cur) != 0)
    return;
  if (!cur)
    read_gpu_uint(gpu, "/gt_cur_freq_mhz", &cur);
  *freq = cur;
  *freq_max = 0;
  read_gpu_uint(gpu, "/gt_max_freq_mhz", freq_max);
  if (gpu_rc6_pct(gpu, pct, ts) == 0)
    return;
  ull gt_max = *freq_max ? *freq_max : 1;
  *pct = cur * PERCENT_BASE / gt_max;
}

static void gpu_read_gov(struct clock_state *ci)
{
  if (!(ci->keep.gpu.present & GPU_HAS_GOV)) {
    *ci->gpu_gov = 0;
    return;
  }
  char buf[sizeof ci->gpu_gov];
  if (read_gpu_file(&ci->keep.gpu, "/device/power_dpm_force_performance_level", buf, sizeof buf) != 0) {
    *ci->gpu_gov = 0;
    return;
  }
  size_t n = strlen(buf);
  if (n && buf[n - 1] == '\n')
    n--;
  memcpy(ci->gpu_gov, buf, n);
  ci->gpu_gov[n] = 0;
}

struct gpu_vals {
  ull pct, temp, freq, freq_max, mf, mfmax;
};

static void gpu_store(struct clock_state *ci, const struct gpu_vals *v)
{
  ci->gpu_pct = (int)v->pct;
  ci->gpu_temp = (int)v->temp;
  if (v->freq || v->freq_max) {
    ci->gpu_freq = (int)v->freq;
    ci->gpu_freq_max = (int)v->freq_max;
  }
  if (v->mf || v->mfmax) {
    ci->gpu_mem_freq = (int)v->mf;
    ci->gpu_mem_freq_max = (int)v->mfmax;
  }
}

static void gpu_read_amd(struct gpu_ctx *g, struct gpu_vals *v, const struct timespec *ts)
{
  if (g->present & GPU_HAS_BUSY_PCT)
    read_gpu_uint(g, "/device/gpu_busy_percent", &v->pct);
  else if (g->present & GPU_HAS_RC6)
    gpu_rc6_pct(g, &v->pct, ts);
  if (g->present & GPU_HAS_PP_SCLK)
    read_amd_freq_pair(g, "/device/pp_dpm_sclk", &v->freq, &v->freq_max);
  if (g->present & GPU_HAS_PP_MCLK)
    read_amd_freq_pair(g, "/device/pp_dpm_mclk", &v->mf, &v->mfmax);
}

void gpu_probe(struct gpu_ctx *g)
{
  find_card(g);
  if (!g->card_path[0])
    return;
  g->present = 0;
  gpu_detect_type(g);
  gpu_find_hwmon(g);
  if (g->hwmon_path[0]) {
    snprintf(g->temp_path, sizeof g->temp_path, "%s/temp1_input", g->hwmon_path);
    snprintf(g->fan_path, sizeof g->fan_path, "%s/fan1_input", g->hwmon_path);
  }
  g->present |= gpu_probe_features(g);
}

void gpu_collect(struct clock_state *ci)
{
  struct gpu_ctx *g = &ci->keep.gpu;
  if (!g->present)
    return;
  struct gpu_vals v = { 0 };
  if (g->present & GPU_HAS_GT_FREQ)
    gpu_read_intel(g, &v.pct, &v.freq, &v.freq_max, &ci->keep.realtime_ts);
  else
    gpu_read_amd(g, &v, &ci->keep.realtime_ts);
  v.temp = gpu_read_temp(g), gpu_read_mem(ci), gpu_store(ci, &v), gpu_read_gov(ci);
}
