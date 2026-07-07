#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

typedef unsigned long long ull;

static ull gpu_read_fan(const struct gpu_ctx *gpu)
{
  if (!(gpu->present & GPU_HAS_FAN))
    return 0;
  ull v = 0;
  read_uint(gpu->fan_path, &v);
  return v;
}

static int gpu_render_mem(char *p, const char *e, unsigned long long total, unsigned long long used, int panel)
{
  int mpct = (int)(used * PERCENT_BASE / total);
  const char *l = panel ? "\xf0\x9f\x8e\x9e\xef\xb8\x8f " : "VRAM ";
  if (total <= (unsigned long long)DISPLAY_UNIT_THRESHOLD * BYTES_PER_MB)
    return snprintf(p, e - p, "%s%d%% %.1f/%.1fM", l, mpct, used / BYTES_PER_MB_F, total / BYTES_PER_MB_F);
  return snprintf(p, e - p, "%s%d%% %.1f/%.1fG", l, mpct, used / BYTES_PER_GB_F, total / BYTES_PER_GB_F);
}

static int fmt_gpu_clock(char *p, const char *e, int freq, int freq_max)
{
  int n = fmt_freq(p + 1, (int)(e - p - 1), freq, freq_max);
  if (n <= 0)
    return 0;
  *p = ' ';
  return n + 1;
}

static char *gpu_render(struct clock_state *ci)
{
  char *p = ci->gpu_line;
  const char *e = p + sizeof ci->gpu_line;
  p += snprintf(p, e - p, "%s %d%%", ci->keep.text ? "GPU" : "\xf0\x9f\x8e\xae", ci->gpu_pct);
  p += fmt_gpu_clock(p, e, ci->gpu_freq, ci->gpu_freq_max);
  if (ci->gpu_temp)
    p += snprintf(p, e - p, " %d\xc2\xb0" "C", ci->gpu_temp);
  return p;
}

static void gpu_render_vram(struct clock_state *ci)
{
  if (!ci->gpu_mem_total)
    return;
  char *p = ci->vram_line;
  const char *e = p + sizeof ci->vram_line;
  p += gpu_render_mem(p, e, ci->gpu_mem_total, ci->gpu_mem_used, !ci->keep.text);
  fmt_gpu_clock(p, e, ci->gpu_mem_freq, ci->gpu_mem_freq_max);
}

static void gpu_render_extra(struct clock_state *ci, char *p)
{
  const char *e = ci->gpu_line + sizeof ci->gpu_line;
  ull fan = gpu_read_fan(&ci->keep.gpu);
  if (fan)
    p += snprintf(p, e - p, " %lluRPM", fan);
  if (ci->gpu_gov[0])
    snprintf(p, e - p, " %s", ci->gpu_gov);
}

void get_gpu_info(struct clock_state *ci)
{
  ci->vram_line[0] = 0;
  *ci->gpu_line = 0;
  *ci->gpu_gov = 0;
  gpu_collect(ci);
  if (!ci->keep.gpu.present)
    return;
  char *end = gpu_render(ci);
  gpu_render_vram(ci);
  gpu_render_extra(ci, end);
}
