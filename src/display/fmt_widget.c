#include <stdio.h>              // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include "main.h"
#include "display/render.h"
#include "display/esc.h"
#include "monitor/widget.h"
#include "monitor/monitor_int.h"
#include "util/syscall.h"

enum { BAT_EST_SZ = 24 };

static int zcat(int n, int sz, int r)
{
  int rem = sz - n;
  return r >= 0 && r < rem ? n + r : sz;
}

static const char *BAT_I[] = { "", " \xf0\x9f\xaa\xab", " \xe2\x9a\xa1", " \xf0\x9f\x94\x8b" };
static const char *BAT_TEXT[] = { "", " discharging", " charging", " full" };

static int fmt_est(char *b, int z, int sec, int discharging)
{
  if (sec < 0)
    return 0;
  const char *arr = discharging ? "\xe2\x86\x93" : "\xe2\x86\x91";
  int h = sec / SECS_PER_HOUR;
  int m = (sec % SECS_PER_HOUR) / SECS_PER_MIN;
  if (h > 0)
    return snprintf(b, z, " %s %dh %dm", arr, h, m);
  return snprintf(b, z, " %s %dm", arr, m);
}

static int cat_lines(char *b, int sz, const char *lines, const char *pfx)
{
  int n = 0;
  while (*lines && n < sz - 1) {
    const char *nl = strchr(lines, '\n');
    if (nl) {
      n = zcat(n, sz, snprintf(b + n, sz - n, "%s%.*s\n", pfx, (int)(nl - lines), lines));
      lines = nl + 1;
    } else {
      int l = strlen(lines);
      n = zcat(n, sz, snprintf(b + n, sz - n, "%s%.*s\n", pfx, l, lines));
      break;
    }
  }
  return n;
}

static int fmt_one_ip(char *b, int z, const char *pfx, const char *tag, const char *v)
{
  return v[0] ? snprintf(b, z, "%s%s %s\n", pfx, tag, v) : 0;
}

static int wan_check(int st, const char *pub, const char *loc)
{
  return !st && pub[0] && strcmp(pub, loc);
}

static int ip_line(char *b, int z, const char *p, const char *icon, const struct clock_state *c)
{
  const char *t4 = icon ? icon : "IP";
  const char *t6 = icon ? icon : "IP6";
  int n = fmt_one_ip(b, z, p, t4, c->local_ip);
  return zcat(n, z, fmt_one_ip(b + n, z - n, p, t6, c->local_ip6));
}

static int wan_line(char *b, int z, const char *p, const char *icon, const struct clock_state *c)
{
  const struct cpu_keep *k = &c->keep;
  int n = 0;
  if (wan_check(k->wan4_state, k->pub_ip4, c->local_ip))
    n = zcat(n, z, fmt_one_ip(b + n, z - n, p, icon ? icon : "WAN", k->pub_ip4));
  if (wan_check(k->wan6_state, k->pub_ip6, c->local_ip6))
    n = zcat(n, z, fmt_one_ip(b + n, z - n, p, icon ? icon : "WAN6", k->pub_ip6));
  return n;
}

typedef int (*fmt_fn)(char *, int, const struct clock_state *, const char *pfx);

static int fmt_date(char *b, int z, const struct clock_state *c, const char *pfx)
{
  return snprintf(b, z, "%s%s\n", pfx, c->date_str);
}

static int fmt_cpu(char *b, int z, const struct clock_state *c, const char *pfx)
{
  int lpct = (int)(c->load / c->num_cpus * PERCENT_BASE);
  const char *l = !c->keep.text ? "\xf0\x9f\x92\xbb " : "CPU ";
  char freq[CUP_BUF_SZ], tmp[16] = "";
  fmt_freq(freq, sizeof freq, c->freq, c->freq_max);
  if (c->temp > 0)
    snprintf(tmp, sizeof tmp, " %d\xc2\xb0" "C", c->temp);
  return snprintf(b, z, "%s%s%d%% %d%% %d%% %s%s %s\n", pfx, l, c->pct, lpct, c->iowait_pct, freq, tmp, c->governor);
}

static int fmt_gpu(char *b, int z, const struct clock_state *c, const char *pfx)
{
  int n = cat_lines(b, z, c->gpu_line, pfx);
  n = zcat(n, z, cat_lines(b + n, z - n, c->vram_line, pfx));
  return n;
}

static int fmt_mem_pair(char *b, int z, int pct, unsigned long long used_kb, unsigned long long total_kb)
{
  if (total_kb / BYTES_PER_KB <= DISPLAY_UNIT_THRESHOLD)
    return snprintf(b, z, "%d%% %.1f/%.1fM", pct, (double)used_kb / BYTES_PER_KB_F, (double)total_kb / BYTES_PER_KB_F);
  return snprintf(b, z, "%d%% %.1f/%.1fG", pct, (double)used_kb / BYTES_PER_MB_F, (double)total_kb / BYTES_PER_MB_F);
}

static int fmt_mem_ctr(char *b, int z, const struct clock_state *c)
{
  if (!c->has_ctr)
    return 0;
  int n = fmt_mem_pair(b, z, c->ctr_pct, c->ctr_used_kb, c->ctr_max_kb);
  int rem = z - n;
  return rem > 0 ? n + snprintf(b + n, rem, " ") : z;
}

static int fmt_mem(char *b, int z, const struct clock_state *c, const char *pfx)
{
  int n = snprintf(b, z, "%s%s", pfx, !c->keep.text ? "\xf0\x9f\xa7\xa0 " : "MEM ");
  n = zcat(n, z, fmt_mem_ctr(b + n, z - n, c));
  n = zcat(n, z, fmt_mem_pair(b + n, z - n, c->mem_pct, c->mem_total_kb - c->mem_avail_kb, c->mem_total_kb));
  n = zcat(n, z, snprintf(b + n, z - n, "\n"));
  return n;
}

static int fmt_fan_panel(char *b, int z, const char *s, const char *pfx)
{
  int n = 0;
  while (*s && n < z - 1) {
    int l = strcspn(s, "\n");
    if (l >= 4 && memcmp(s, "FAN ", 4) == 0)
      n = zcat(n, z, snprintf(b + n, z - n, "%s\xf0\x9f\x92\xa8 %.*s\n", pfx, l - 4, s + 4));
    else
      n = zcat(n, z, snprintf(b + n, z - n, "%s\xf0\x9f\x8c\xa1\xef\xb8\x8f %.*s\n", pfx, l, s));
    s += l + !!s[l];
  }
  return n;
}

static int fmt_fan(char *b, int z, const struct clock_state *c, const char *pfx)
{
  if (c->keep.text)
    return cat_lines(b, z, c->fan_line, pfx);
  return fmt_fan_panel(b, z, c->fan_line, pfx);
}

static int fmt_bat(char *b, int z, const struct clock_state *c, const char *pfx)
{
  if (c->bat_pct < 0)
    return 0;
  int bv = c->bat_charging;
  char est[BAT_EST_SZ];
  int en = fmt_est(est, sizeof est, c->bat_est_sec, bv == 0);
  if (!c->keep.text) {
    const char *ic = BAT_I[bv + 1];
    return snprintf(b, z, "%s%s %d%%%.*s\n", pfx, ic + (*ic == ' '), c->bat_pct, en, est);
  }
  return snprintf(b, z, "%sBAT %d%%%.*s%s\n", pfx, c->bat_pct, en, est, BAT_TEXT[bv + 1]);
}

static int fmt_up(char *b, int z, const struct clock_state *c, const char *pfx)
{
  return snprintf(b, z, "%s%s%dd %dh %dm\n", pfx, !c->keep.text ? "\xe2\x8f\xb1\xef\xb8\x8f " : "UP ", c->uptime_d,
                  c->uptime_h, c->uptime_m);
}

static int fmt_sto(char *b, int z, const struct clock_state *c, const char *pfx)
{
  return cat_lines(b, z, c->sto_line, pfx);
}

static int fmt_net(char *b, int z, const struct clock_state *c, const char *pfx)
{
  int n = 0;
  if (*c->net_line)
    n = zcat(n, z, cat_lines(b + n, z - n, c->net_line, pfx));
  const char *ip_icon = !c->keep.text ? "\xf0\x9f\x93\xa1" : NULL;
  const char *wan_icon = !c->keep.text ? "\xf0\x9f\x8c\x8d" : NULL;
  n = zcat(n, z, ip_line(b + n, z - n, pfx, ip_icon, c));
  n = zcat(n, z, wan_line(b + n, z - n, pfx, wan_icon, c));
  return n;
}

static const char *weather_label(const struct clock_state *c)
{
  if (c->keep.text)
    return c->weather_desc;
  const char *emo = weather_emoji(c->weather_code);
  return emo[0] ? emo : c->weather_desc;
}

static int fmt_weather(char *b, int z, const struct clock_state *c, const char *pfx)
{
  if (c->keep.weather_state || !c->keep.weather_valid)
    return snprintf(b, z, "%s-\n", pfx);
  if (c->weather_temp == 0 && !c->weather_desc[0])
    return 0;
  const char *label = weather_label(c);
  return snprintf(b, z, "%s%s %+d\xc2\xb0" "C [%+d\xc2\xb0" "C..%+d\xc2\xb0" "C]\n",
                  pfx, label, c->weather_temp, c->weather_min, c->weather_max);
}

static int fmt_cal(char *b, int z, const struct clock_state *c, const char *pfx)
{
  return cat_lines(b, z, c->cal_grid, pfx);
}

static const fmt_fn WIDGET_FNS[WIDGET_COUNT] = {
  [WIDGET_DATE] = fmt_date,
  [WIDGET_CPU] = fmt_cpu,
  [WIDGET_GPU] = fmt_gpu,
  [WIDGET_MEM] = fmt_mem,
  [WIDGET_FAN] = fmt_fan,
  [WIDGET_BAT] = fmt_bat,
  [WIDGET_UP] = fmt_up,
  [WIDGET_STO] = fmt_sto,
  [WIDGET_NET] = fmt_net,
  [WIDGET_WEATHER] = fmt_weather,
  [WIDGET_CAL] = fmt_cal,
};

int fmt_widget_list(char *b, int z, const struct clock_state *ci, const char *pfx)
{
  int wc;
  const WidgetType *set = widget_get_active(&ci->keep.widget, &wc);
  int n = 0;
  for (int i = 0; i < wc; i++) {
    fmt_fn fn = WIDGET_FNS[set[i]];
    if (fn)
      n = zcat(n, z, fn(b + n, z - n, ci, pfx));
  }
  return n;
}

int render_panel_buf(char *buf, int sz, const struct clock_state *ci)
{
  int n = fmt_widget_list(buf, sz, ci, ESC_EL);
  n = zcat(n, sz, snprintf(buf + n, sz - n, ESC_EL "\n"));
  return n;
}

void set_date_str(struct clock_state *ci, const struct tm *tm)
{
  strftime(ci->date_str, sizeof ci->date_str, "%a %d %b %Y", tm);
}

void render_text_info(int h, int m, int s, const struct clock_state *ci)
{
  char buf[RENDER_BUF];
  int n = snprintf(buf, sizeof buf, "%02d:%02d:%02d\n", h, m, s);
  n = zcat(n, (int)sizeof buf, fmt_widget_list(buf + n, (int)sizeof buf - n, ci, ""));
  sys_write(STDOUT_FILENO, buf, n);
}
