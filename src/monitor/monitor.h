#ifndef MONITOR_H
#define MONITOR_H

#include "monitor_int.h"
#include <time.h>               // cppcheck-suppress missingIncludeSystem

struct bat_sample {
  int duration_sec;
  long long power_diff;
  int biased;
};

struct async_ctx {
  struct dns_slot wan4_dns;
  struct dns_slot wan6_dns;
  struct dns_slot weather_dns;
  struct http_result wan4_result;
  struct http_result wan6_result;
  struct http_result weather_result;
  struct ioserv_ctl ioc;
  unsigned int widget_mask;
  int *wan4_fd;
  int *wan6_fd;
  int *weather_fd;
};

struct fentry;

struct cpu_keep {
  char pub_ip4[IP4_SZ];
  char pub_ip6[IP6_SZ];
  struct net_ctx net;
  struct disk_ctx disk;
  struct gpu_ctx gpu;
  struct widget_ctx widget;
  struct netlink_ctx nlk;
  struct rtnl_ctx rtnl;
  int text;
  enum wan_state wan4_state;
  enum wan_state wan6_state;
  int wan4_fd;
  int wan6_fd;
  enum wan_state weather_state;
  char weather_valid;
  int weather_fd;
  int weather_temp;
  int weather_max;
  int weather_min;
  int weather_code;
  char weather_desc[DESC_LEN];
  char ipv4_local[IP4_SZ];
  char ipv6_local[IP6_SZ];
  unsigned long long v4_ip_refresh_ts;
  unsigned long long v6_ip_refresh_ts;
  int ip_refresh_sec;
  struct rtnl_mon_ctx rtnl_mon;
  struct async_ctx async;
  unsigned long long weather_refresh_ts;
  int weather_refresh_sec;
  struct fetch_retry wan4;
  struct fetch_retry wan6;
  struct fetch_retry weather;
  struct sysinfo si;
  struct timespec realtime_ts;
  unsigned long long cpu_prev_idle;
  unsigned long long cpu_prev_total;
  unsigned long long cpu_prev_iowait;
  char bat_prefix[PATH_BUF_SZ];
  struct bat_sample bat_samples_chg[BAT_NUM_SAMPLES];
  struct bat_sample bat_samples_dchg[BAT_NUM_SAMPLES];
  int bat_idx_chg;
  int bat_idx_dchg;
  int bat_charge_count;
  int bat_discharge_count;
  int bat_last_raw;
  int bat_tstate;
  int bat_prev_state;
  time_t bat_change_ts;
  int bat_charge_full_raw;
  int bat_biased_next_chg;
  int bat_biased_next_dchg;
  int bat_unbiased_full_chg;
  int bat_unbiased_full_dchg;
  int bat_inited;
  unsigned int bat_flags;
  unsigned int mem_flags;
  char cpu_temp_path[CPU_TEMP_PATH_SZ];
  char **cpu_freq_dirs;
  unsigned cpu_freq_ndir;
  int cpu_freq_ndir_cap;
  char mobo_hwmon[PATH_BUF_SZ];
  char *fan_fb_valid;
  int fan_fb_cap;
  struct fentry *fan_ents;
  int fan_n;
  int fan_cap;
  char fan_cache_dir[PATH_BUF_SZ];
};

struct clock_state {
  int pct;
  int iowait_pct;
  int num_cpus;
  double load;
  char governor[GOVERNOR_SZ];
  int freq;
  int freq_max;
  int temp;
  int mem_pct;
  unsigned long long mem_total_kb;
  unsigned long long mem_avail_kb;
  int uptime_d;
  int uptime_h;
  int uptime_m;
  int bat_pct;
  int bat_charging;
  int bat_est_sec;
  int net_dbm;
  int gpu_pct;
  int gpu_temp;
  int gpu_freq;
  int gpu_freq_max;
  int gpu_mem_freq;
  int gpu_mem_freq_max;
  unsigned long long gpu_mem_used;
  unsigned long long gpu_mem_total;
  char gpu_gov[GOVERNOR_SZ];
  int ctr_pct;
  unsigned long long ctr_used_kb;
  unsigned long long ctr_max_kb;
  char gpu_line[PATH_SZ];
  char vram_line[PATH_SZ];
  char fan_line[PATH_SZ];
  int weather_temp;
  int weather_max;
  int weather_min;
  int weather_code;
  char weather_desc[DESC_LEN];
  int has_ctr;
  char net_line[BIG_BUF];
  char date_str[DESC_LEN];
  char cal_grid[PATH_SZ];
  char sto_line[BIG_BUF];
  char local_ip[IP4_SZ];
  char local_ip6[IP6_SZ];
  struct cpu_keep keep;
};

void gather_all(struct clock_state *ci, time_t now);
void discover_hardware(struct clock_state *ci);
void async_ctx_init(struct async_ctx *ctx, int *wan4_fd, int *wan6_fd, int *weather_fd, unsigned int widget_mask);

#endif
