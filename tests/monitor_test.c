#define _GNU_SOURCE
#include "mock_syscall.h"
#include "monitor/monitor.h"
#include "monitor/monitor_int.h"
#include "monitor/disk_impl.h"
#include "monitor/widget.h"
#include "util/syscall.h"
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <stddef.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <string.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <netinet/in.h>         // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <stdint.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <linux/netlink.h>      // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <linux/rtnetlink.h>    // cppcheck-suppress missingIncludeSystem -- musl include paths not known

#include <linux/if_link.h>      // cppcheck-suppress missingIncludeSystem -- for rtnl_link_stats64
#include <linux/genetlink.h>    // cppcheck-suppress missingIncludeSystem -- for genlmsghdr
#include <linux/nl80211.h>      // cppcheck-suppress missingIncludeSystem -- for NL80211 constants
#include <linux/if_addr.h>      // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <arpa/inet.h>          // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <netdb.h>              // cppcheck-suppress missingIncludeSystem
#include <sys/eventfd.h>        // cppcheck-suppress missingIncludeSystem
#include <poll.h>               // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include "display/render.h"     // for fmt_widget_list

/* netlink response storage — separate buffers so mock_add_nl_resp (which stores
   pointers, not copies) is not corrupted by subsequent writes to the same buffer. */
static unsigned char done_buf[16];
static unsigned char route_buf1[128];
static unsigned char route_buf2[128];
static unsigned char link_lo_buf[384];
static unsigned char link_buf1[384];
static unsigned char link_buf2[384];
static unsigned char genl_resp[64];
static unsigned char ssid_buf[128];
static unsigned char stresp_buf[256];
static unsigned char sts_resp[256];

static void test_get_cpu_info(struct clock_state *ci, time_t now);

#include "nl_helper.h"

/* NL80211 constants not available in test scope */
/* genetlink.h enum values — match system headers */
#define CTRL_CMD_NEWFAMILY   1
#define CTRL_ATTR_FAMILY_ID  1

/* Reusable genl response builder state */
#define GENBUF 1024
static unsigned char genbuf[GENBUF];
static struct nlmsghdr *rnh;
static struct genlmsghdr *rgh;

static void mkgenl(unsigned short family, unsigned char cmd)
{
  memset(genbuf, 0, sizeof genbuf);
  rnh = (struct nlmsghdr *)genbuf;
  rgh = (struct genlmsghdr *)(genbuf + sizeof(struct nlmsghdr));
  rnh->nlmsg_type = family;
  rgh->cmd = cmd;
  rgh->version = 1;
  rnh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
}

static void add_nla(int type, int paylen)
{
  unsigned char *p = (unsigned char *)rgh + GENL_HDRLEN;
  size_t off = 0;
  while (off < sizeof genbuf - 64) {
    struct nlattr *a = (struct nlattr *)(p + off);
    if (a->nla_type == 0 && a->nla_len == 0)
      break;
    off += NLA_ALIGN(a->nla_len);
  }
  struct nlattr *a = (struct nlattr *)(p + off);
  a->nla_type = (unsigned short)type;
  a->nla_len = (unsigned short)(NLA_HDRLEN + paylen);
  rnh->nlmsg_len = (unsigned)(NLMSG_LENGTH(GENL_HDRLEN + off + NLA_ALIGN(a->nla_len)));
}

static void mk_genl_resolve(unsigned short family_id)
{
  mkgenl(GENL_ID_CTRL, CTRL_CMD_NEWFAMILY);
  add_nla(CTRL_ATTR_FAMILY_ID, 2);
  *(unsigned short *)((char *)((struct nlattr *)((unsigned char *)rgh + GENL_HDRLEN)) + NLA_HDRLEN) = family_id;
}

static void mk_sta_resp(int dbm)
{
  mkgenl(28, NL80211_CMD_NEW_STATION);
  rnh->nlmsg_flags = NLM_F_MULTI;
  unsigned char *p = (unsigned char *)rgh + GENL_HDRLEN;
  struct nlattr *si = (struct nlattr *)p;
  si->nla_type = NL80211_ATTR_STA_INFO;
  struct nlattr *sg = (struct nlattr *)(p + NLA_HDRLEN);
  sg->nla_type = NL80211_STA_INFO_SIGNAL;
  sg->nla_len = NLA_HDRLEN + 1;
  *(signed char *)((char *)sg + NLA_HDRLEN) = (signed char)dbm;
  si->nla_len = (unsigned short)(NLA_HDRLEN + (size_t)((char *)(sg + 1) - (char *)si));
  rnh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN + NLA_ALIGN(si->nla_len));
}

static void mk_wlan_dump_resp(const char *name, const char *ssid, int ifindex)
{
  mkgenl(28, NL80211_CMD_NEW_INTERFACE);
  rnh->nlmsg_flags = NLM_F_MULTI;
  unsigned char *p = (unsigned char *)rgh + GENL_HDRLEN;
  size_t off = 0;

  struct nlattr *ai = (struct nlattr *)(p + off);
  ai->nla_type = NL80211_ATTR_IFINDEX;
  ai->nla_len = NLA_HDRLEN + (unsigned short)sizeof(int);
  *(int *)(p + off + NLA_HDRLEN) = ifindex;
  off += NLA_ALIGN(ai->nla_len);

  struct nlattr *at = (struct nlattr *)(p + off);
  at->nla_type = NL80211_ATTR_IFTYPE;
  at->nla_len = NLA_HDRLEN + (unsigned short)sizeof(int);
  *(int *)(p + off + NLA_HDRLEN) = NL80211_IFTYPE_STATION;
  off += NLA_ALIGN(at->nla_len);

  size_t sl = strlen(name) + 1;
  struct nlattr *an = (struct nlattr *)(p + off);
  an->nla_type = NL80211_ATTR_IFNAME;
  an->nla_len = NLA_HDRLEN + (unsigned short)sl;
  memcpy(p + off + NLA_HDRLEN, name, sl);
  off += NLA_ALIGN(an->nla_len);

  if (ssid) {
    struct nlattr *as = (struct nlattr *)(p + off);
    as->nla_type = NL80211_ATTR_SSID;
    as->nla_len = NLA_HDRLEN + (unsigned short)strlen(ssid);
    memcpy(p + off + NLA_HDRLEN, ssid, strlen(ssid));
    off += NLA_ALIGN(as->nla_len);
  }

  rnh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN + (unsigned)off);
}

/* Set up netlink mocks for a wired-only test (eth0, ifindex=3, metric=100).
   Queue order: link dump first (to populate cache for compact_virtuals),
   then route dump, then targeted query spares. */
static void setup_netlink_wired(unsigned long long rx, unsigned long long tx)
{
  mock_set_ifname(1, "lo");
  mock_set_ifname(3, "eth0");
  mock_set_ifindex(3);

  mk_link_nl(link_lo_buf, "lo", 1, 0, 0, NULL);
  mk_link_nl(link_buf1, "eth0", 3, rx, tx, NULL);
  mk_done_nl(done_buf);
  mock_set_netlink(99, link_lo_buf, ((struct nlmsghdr *)link_lo_buf)->nlmsg_len, link_buf1,
                   ((struct nlmsghdr *)link_buf1)->nlmsg_len);
  mock_add_nl_resp(done_buf, sizeof(struct nlmsghdr));
  mk_route_nl(route_buf1, AF_INET, 3, 100);
  mk_done_nl(done_buf);
  mock_add_nl_resp(route_buf1, ((struct nlmsghdr *)route_buf1)->nlmsg_len);
  mock_add_nl_resp(done_buf, sizeof(struct nlmsghdr));
  mock_add_nl_resp(done_buf, sizeof(struct nlmsghdr));
}

/* Set up netlink mocks for a two-interface scenario (eth0 + wlan0).
   eth0: ifindex=3, metric=100
   wlan0: ifindex=2, metric=200
   The wireless detection via nl80211 dump is fully mocked.
   rx/tx values are the per-interface link stats. dbm is the signal strength. */
static void setup_netlink_full(unsigned long long rx_eth, unsigned long long tx_eth,
                               unsigned long long rx_wlan, unsigned long long tx_wlan, int dbm)
{
  mock_set_ifname(1, "lo");
  mock_set_ifname(3, "eth0");
  mock_set_ifname(2, "wlan0");
  mock_set_ifindex(3);

  /* Link cache (rtnl_cache_links via get_net_info) — must come before
     route dump so compact_virtuals finds populated cache */
  mk_link_nl(link_lo_buf, "lo", 1, 0, 0, NULL);
  mk_link_nl(link_buf1, "eth0", 3, rx_eth, tx_eth, NULL);
  mk_link_nl(link_buf2, "wlan0", 2, rx_wlan, tx_wlan, "wlan");
  mk_done_nl(done_buf);
  mock_set_netlink(99, link_lo_buf, ((struct nlmsghdr *)link_lo_buf)->nlmsg_len,
                   link_buf1, ((struct nlmsghdr *)link_buf1)->nlmsg_len);
  mock_add_nl_resp(link_buf2, ((struct nlmsghdr *)link_buf2)->nlmsg_len);
  mock_add_nl_resp(done_buf, sizeof(struct nlmsghdr));

  /* route dump: two routes before DONE so rtnl_recv_loop consumes all */
  mk_route_nl(route_buf1, AF_INET, 3, 100);
  mk_route_nl(route_buf2, AF_INET, 2, 200);
  mk_done_nl(done_buf);
  mock_add_nl_resp(route_buf1, ((struct nlmsghdr *)route_buf1)->nlmsg_len);
  mock_add_nl_resp(route_buf2, ((struct nlmsghdr *)route_buf2)->nlmsg_len);
  mock_add_nl_resp(done_buf, sizeof(struct nlmsghdr));
  /* spare DONE so v6 route dump terminates without consuming genl responses */
  mock_add_nl_resp(done_buf, sizeof(struct nlmsghdr));

  /* genl resolve (find_wlan_nlk → nlk_find_wlan_all → ensure_resolved) */
  mk_genl_resolve(28);
  memcpy(genl_resp, genbuf, rnh->nlmsg_len);
  mock_add_nl_resp(genl_resp, rnh->nlmsg_len);

  /* nl80211 dump response: wlan0 with ifindex, iftype=station, ssid */
  mk_wlan_dump_resp("wlan0", "MyWiFi", 2);
  memcpy(ssid_buf, genbuf, rnh->nlmsg_len);
  mock_add_nl_resp(ssid_buf, rnh->nlmsg_len);
  mk_done_nl(done_buf);
  mock_add_nl_resp(done_buf, sizeof(struct nlmsghdr));

  /* Station rate: first non-dump talk, then dump talk_dump */
  mk_sta_resp(dbm);
  memcpy(sts_resp, genbuf, rnh->nlmsg_len);
  mock_add_nl_resp(sts_resp, rnh->nlmsg_len);
  mk_sta_resp(dbm);
  memcpy(stresp_buf, genbuf, rnh->nlmsg_len);
  mock_add_nl_resp(stresp_buf, rnh->nlmsg_len);
  mk_done_nl(done_buf);
  mock_add_nl_resp(done_buf, sizeof(struct nlmsghdr));
}

static void setup_normal_mocks(void)
{
  mock_reset();
  struct sysinfo si = {.uptime = 123456,.loads = {80609, 0, 0} };
  mock_set_sysinfo(&si, 0);
  mock_file("/proc/stat", "cpu  1000 0 0 0 0 0 0 0 0 0\n");
  mock_glob("/sys/devices/system/cpu/cpu*/cpufreq", (char *[]) { (char *)"/sys/devices/system/cpu/cpu0/cpufreq" }, 1);
  mock_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "2200000\n");
  mock_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "3700000\n");
  mock_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "powersave\n");
  mock_glob("/sys/devices/platform/coretemp.0/hwmon/hwmon*/temp1_input",
            (char *[]) { (char *)"/sys/devices/platform/coretemp.0/hwmon/hwmon2/temp1_input" }, 1);
  mock_file("/sys/devices/platform/coretemp.0/hwmon/hwmon2/temp1_input", "45000\n");
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4250000\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Discharging\n");
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/proc/meminfo",
            "MemTotal:      16384000 kB\n"
            "MemFree:        8192000 kB\n"
            "MemAvailable:   9216000 kB\n" "Buffers:        1000000 kB\n" "Cached:         5000000 kB\n");
  setup_netlink_full(1048576, 524288, 2097152, 1048576, -45);

  mock_glob("/sys/block/*", (char *[]) { (char *)"/sys/block/sda",
            (char *)"/sys/block/nvme0n1"
            }, 2);
  mock_file("/sys/block/sda/stat", " 100 0 50000 0 200 0 60000 0 0 0 0\n");
  mock_file("/sys/block/sda/dev", "8:0\n");
  mock_file("/sys/block/nvme0n1/stat", " 50 0 20000 0 100 0 40000 0 0 0 0\n");
  mock_file("/sys/block/nvme0n1/dev", "259:0\n");
  mock_file("/sys/fs/cgroup/memory.current", "104857600\n");
  mock_file("/sys/fs/cgroup/memory.max", "209715200\n");
  mock_glob("/sys/class/hwmon/hwmon*", (char *[]) { (char *)"/sys/class/hwmon/hwmon3",
            (char *)"/sys/class/hwmon/hwmon4",
            (char *)"/sys/class/hwmon/hwmon5",
            (char *)"/sys/class/hwmon/hwmon6",
            (char *)"/sys/class/hwmon/hwmon7",
            (char *)"/sys/class/hwmon/hwmon8"
            }, 6);
  mock_file("/sys/class/hwmon/hwmon3/name", "nct6798\n");
  mock_file("/sys/class/hwmon/hwmon4/name", "nct6798\n");
  mock_file("/sys/class/hwmon/hwmon5/name", "k10temp\n");
  mock_file("/sys/class/hwmon/hwmon6/name", "nct6798\n");
  mock_glob("/sys/class/hwmon/hwmon3/*_input", (char *[]) { (char *)"/sys/class/hwmon/hwmon3/fan1_input" }, 1);
  mock_file("/sys/class/hwmon/hwmon3/fan1_input", "2200\n");
  mock_glob("/sys/class/hwmon/hwmon4/*_input", (char *[]) { (char *)"/sys/class/hwmon/hwmon4/temp1_input",
            (char *)"/sys/class/hwmon/hwmon4/temp2_input"
            }, 2);
  mock_file("/sys/class/hwmon/hwmon4/temp1_input", "38000\n");
  mock_file("/sys/class/hwmon/hwmon4/temp2_input", "42000\n");
  mock_glob("/sys/class/hwmon/hwmon6/*_input", (char *[]) { (char *)"/sys/class/hwmon/hwmon6/fan1_input",
            (char *)"/sys/class/hwmon/hwmon6/temp1_input"
            }, 2);
  mock_file("/sys/class/hwmon/hwmon6/fan1_input", "1500\n");
  mock_file("/sys/class/hwmon/hwmon6/temp1_input", "40000\n");
  mock_file("/sys/class/hwmon/hwmon8/name", "nct6798\n");
  mock_glob("/sys/class/hwmon/hwmon8/*_input", (char *[]) { (char *)"/sys/class/hwmon/hwmon8/fan1_input" }, 1);
  mock_glob("/sys/class/drm/card[0-9]*", (char *[]) { (char *)"/sys/class/drm/card0" }, 1);
  mock_glob("/sys/class/drm/card0/device/gpu_busy_percent",
            (char *[]) { (char *)"/sys/class/drm/card0/device/gpu_busy_percent" }, 1);
  mock_file("/sys/class/drm/card0/device/gpu_busy_percent", "42\n");
  mock_file("/sys/class/drm/card0/device/pp_dpm_sclk", "0: 300Mhz *\n1: 1200Mhz\n2: 2000Mhz\n");
  mock_file("/sys/class/drm/card0/device/pp_dpm_mclk", "0: 100Mhz *\n1: 800Mhz\n2: 1200Mhz\n");
  mock_file("/sys/class/drm/card0/device/power_dpm_force_performance_level", "high\n");
  mock_glob("/sys/class/drm/card0/device/hwmon/hwmon*",
            (char *[]) { (char *)"/sys/class/drm/card0/device/hwmon/hwmon1" }, 1);
  mock_file("/sys/class/drm/card0/device/hwmon/hwmon1/temp1_input", "55000\n");
  mock_file("/sys/class/drm/card0/device/hwmon/hwmon1/fan1_input", "3000\n");
  mock_file("/sys/class/drm/card0/device/mem_info_vis_vram_total", "8388608\n");
  mock_file("/sys/class/drm/card0/device/mem_info_vis_vram_used", "2097152\n");
}

static int check_norm_cpu(const struct clock_state *ci)
{
  if (ci->pct != 0)
    return 1;
  if (ci->load < 1.22 || ci->load > 1.24)
    return 2;
  if (ci->freq != 2200)
    return 3;
  if (ci->freq_max != 3700)
    return 4;
  if (ci->temp != 45)
    return 5;
  if (strcmp(ci->governor, "powersave") != 0)
    return 6;
  return 0;
}

static int check_norm_bat_up(const struct clock_state *ci)
{
  if (ci->bat_pct != 85)
    return 7;
  if (ci->bat_charging != 0)
    return 8;
  if (ci->uptime_d != 1)
    return 9;
  if (ci->uptime_h != 10)
    return 10;
  if (ci->uptime_m != 17)
    return 11;
  if (ci->mem_pct != 43)
    return 12;
  return 0;
}

static int check_norm_net_gpu(struct clock_state *ci)
{
  if (!ci->net_line[0])
    return 0;
  if (strstr(ci->net_line, "eth0") == NULL)
    return 13;
  if (strstr(ci->net_line, "wlan0") == NULL)
    return 14;
  if (ci->net_dbm != -45)
    return 15;
  if (ci->ctr_pct != 50)
    return 16;
  if (!ci->has_ctr)
    return 17;
  if (ci->gpu_pct != 42)
    return 18;
  return 0;
}

static int check_norm_fan(struct clock_state *ci)
{
  if (ci->gpu_temp != 55)
    return 19;
  if (strstr(ci->gpu_line, "GPU 42%") == NULL)
    return 20;
  if (strstr(ci->vram_line, "VRAM 25% 2.0/8.0M") == NULL)
    return 21;
  if (strstr(ci->fan_line, "FAN 2200RPM") == NULL)
    return 22;
  if (strstr(ci->fan_line, "38\xc2" "\xb0" "C") == NULL)
    return 23;
  if (strstr(ci->fan_line, "42\xc2" "\xb0" "C") == NULL)
    return 24;
  return 0;
}

static int test_fan_stale_newline(void)
{
  struct clock_state ci = { 0 };
  setup_normal_mocks();
  test_get_cpu_info(&ci, 0);
  size_t len = strlen(ci.fan_line);
  if (len == 0 || ci.fan_line[len - 1] != '\n')
    return 1;

  ci.fan_line[len] = '\n';
  test_get_cpu_info(&ci, 0);

  size_t len2 = strlen(ci.fan_line);
  if (len2 < 1 || ci.fan_line[len2 - 1] != '\n')
    return 2;
  if (len2 >= 2 && ci.fan_line[len2 - 2] == '\n')
    return 3;

  return 0;
}

static int test_mobo_fan(void)
{
  struct clock_state ci = { 0 };
  setup_normal_mocks();
  mock_glob("/sys/bus/platform/drivers/nct6687/*/hwmon/hwmon*",
            (char *[]) { (char *)"/sys/bus/platform/drivers/nct6687/0000:00:00.0/hwmon/hwmon8" }, 1);
  mock_glob("/sys/bus/platform/drivers/nct6687/0000:00:00.0/hwmon/hwmon8/*_input",
            (char *[]) { (char *)"/sys/bus/platform/drivers/nct6687/0000:00:00.0/hwmon/hwmon8/fan1_input",
            (char *)"/sys/bus/platform/drivers/nct6687/0000:00:00.0/hwmon/hwmon8/temp1_input"
            }, 2);
  mock_file("/sys/bus/platform/drivers/nct6687/0000:00:00.0/hwmon/hwmon8/fan1_input", "1800\n");
  mock_file("/sys/bus/platform/drivers/nct6687/0000:00:00.0/hwmon/hwmon8/temp1_input", "35000\n");
  test_get_cpu_info(&ci, 0);
  if (strstr(ci.fan_line, "FAN 1800RPM") == NULL)
    return 1;
  if (strstr(ci.fan_line, "35\xc2\xb0" "C") == NULL)
    return 2;
  return 0;
}

static int test_normal(void)
{
  struct clock_state ci = { 0 };
  ci.keep.text = 1;
  setup_normal_mocks();
  test_get_cpu_info(&ci, 0);
  int r;
  if ((r = check_norm_cpu(&ci)))
    return r;
  if ((r = check_norm_bat_up(&ci)))
    return r;
  if ((r = check_norm_net_gpu(&ci)))
    return r;
  if ((r = check_norm_fan(&ci)))
    return r;
  return 0;
}

static int test_cached_paths(void)
{
  struct clock_state ci = { 0 };
  ci.keep.text = 1;
  setup_normal_mocks();
  test_get_cpu_info(&ci, 0);
  test_get_cpu_info(&ci, 0);
  test_get_cpu_info(&ci, 0);
  if (ci.temp != 45)
    return 1;
  if (strstr(ci.sto_line, "sda") == NULL)
    return 2;
  if (strstr(ci.sto_line, "nvme0n1") == NULL)
    return 3;
  return 0;
}

static int check_miss_fields1(const struct clock_state *ci)
{
  if (ci->pct != 0)
    return 1;
  if (ci->load != 0.0)
    return 2;
  if (ci->freq != 0)
    return 3;
  if (ci->freq_max != 0)
    return 4;
  if (ci->temp != -1)
    return 5;
  if (ci->bat_pct != -1)
    return 6;
  return 0;
}

static int check_miss_fields2(const struct clock_state *ci)
{
  if (ci->uptime_d != 0)
    return 7;
  if (ci->uptime_h != 0)
    return 8;
  if (ci->uptime_m != 0)
    return 9;
  if (ci->net_line[0] != 0)
    return 10;
  if (ci->sto_line[0] != 0)
    return 11;
  if (ci->has_ctr)
    return 12;
  if (ci->sto_temp != -1)
    return 13;
  return 0;
}

static int test_missing_files(void)
{
  mock_reset();
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  int r;
  if ((r = check_miss_fields1(&ci)))
    return r;
  if ((r = check_miss_fields2(&ci)))
    return r;
  return 0;
}

static int test_container_unlimited(void)
{
  struct clock_state ci = { 0 };
  ci.keep.text = 1;
  setup_normal_mocks();
  mock_file("/sys/fs/cgroup/memory.current", "8589934592\n");
  mock_file("/sys/fs/cgroup/memory.max", "max\n");
  test_get_cpu_info(&ci, 0);
  if (!ci.has_ctr)
    return 1;
  if (ci.ctr_pct != 51)
    return 2;
  return 0;
}

static int test_cpu_delta(void)
{
  mock_reset();
  mock_file("/proc/stat", "cpu  1000 0 0 0 0 0 0 0 0 0\n"
            "cpu0 200 100 50 50 0 0 0 0 0 0\n" "cpu1 200 100 50 50 0 0 0 0 0 0\n");
  mock_file("/proc/meminfo", "MemTotal:      16384000 kB\n" "MemAvailable:  10000000 kB\n");

  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);

  mock_file("/proc/stat", "cpu  1000 0 300 600 100 0 0 0 0 0\n"
            "cpu0 200 100 50 50 0 0 0 0 0 0\n" "cpu1 200 100 50 50 0 0 0 0 0 0\n");

  test_get_cpu_info(&ci, 0);

  if (ci.pct != 40)
    return 1;
  if (ci.iowait_pct != 10)
    return 2;
  if (ci.num_cpus != 2)
    return 3;
  return 0;
}

static int test_battery_energy_now(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) {
            (char *)"/sys/class/power_supply/ACAD/type",
            (char *)"/sys/class/power_supply/BAT0/type"
            }, 2);
  mock_file("/sys/class/power_supply/ACAD/type", "Mains\n");
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/energy_now", "2500000\n");
  mock_file("/sys/class/power_supply/BAT0/energy_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Charging\n");
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (ci.bat_pct != 50)
    return 1;
  if (ci.bat_charging != 1)
    return 2;
  return 0;
}

static int test_battery_charging(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/charge_now", "2500000\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Charging\n");
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (ci.bat_pct != 50)
    return 1;
  if (ci.bat_charging != 1)
    return 2;
  return 0;
}

static int test_battery_full(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/charge_now", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Full\n");
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (ci.bat_charging != 2)
    return 1;
  return 0;
}

static int test_battery_not_charging(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/charge_now", "3750000\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Not charging\n");
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (ci.bat_pct != 75)
    return 1;
  if (ci.bat_charging != 2)
    return 2;
  return 0;
}

static int test_no_battery(void)
{
  mock_reset();
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (ci.bat_pct != -1)
    return 1;
  if (ci.bat_charging != -1)
    return 2;
  return 0;
}

static int test_battery_recalibrate_full(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4800000\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Not charging\n");
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (ci.bat_pct != 96)
    return 1;
  if (ci.keep.bat_charge_full_raw != 5000000)
    return 2;
  /* transition: discharging → not charging triggers recalibration */
  mock_file("/sys/class/power_supply/BAT0/status", "Discharging\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "6000000\n");
  test_get_cpu_info(&ci, 0);
  mock_file("/sys/class/power_supply/BAT0/status", "Not charging\n");
  test_get_cpu_info(&ci, 0);
  if (ci.bat_pct != 80)
    return 3;
  if (ci.keep.bat_charge_full_raw != 6000000)
    return 4;
  return 0;
}

static int est_init(struct clock_state *ci, int raw)
{
  mock_file("/sys/class/power_supply/BAT0/charge_now",
            raw == 4250000 ? "4250000\n" : raw == 5000000 ? "5000000\n" : "000\n");
  test_get_cpu_info(ci, 1000);
  if (ci->bat_pct < 0)
    return 1;
  if (ci->bat_est_sec != -1)
    return 2;
  return 0;
}

static int est_tick(struct clock_state *ci, int raw, time_t sec)
{
  char buf[32];
  snprintf(buf, sizeof buf, "%d\n", raw);
  mock_file("/sys/class/power_supply/BAT0/charge_now", buf);
  test_get_cpu_info(ci, sec);
  return 0;
}

static int test_battery_est_discharge(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Discharging\n");
  struct clock_state ci = { 0 };
  if (est_init(&ci, 4250000))
    return 1;
  /* power drops 10000 µAh over 60 s → est = 4240000*60/10000 = 25440 */
  if (est_tick(&ci, 4240000, 1060))
    return 2;
  if (ci.bat_pct != 84)
    return 3;
  if (ci.bat_est_sec < 25000 || ci.bat_est_sec > 26000)
    return 4;
  return 0;
}

static int test_battery_est_charge(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Charging\n");
  struct clock_state ci = { 0 };
  if (est_init(&ci, 4250000))
    return 1;
  /* power rises 50000 µAh over 60 s; remaining = 5000000 - 4300000 = 700000 */
  /* est = 700000*60/50000 = 840 */
  if (est_tick(&ci, 4300000, 1060))
    return 2;
  if (ci.bat_pct != 86)
    return 3;
  if (ci.bat_est_sec < 800 || ci.bat_est_sec > 880)
    return 4;
  return 0;
}

static int test_battery_est_flat(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Discharging\n");
  struct clock_state ci = { 0 };
  if (est_init(&ci, 4250000))
    return 1;
  /* same charge value → power_diff == 0 → no slot, no estimate */
  if (est_tick(&ci, 4250000, 1060))
    return 2;
  if (ci.bat_est_sec != -1)
    return 3;
  return 0;
}

static int test_battery_est_zero_power(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Discharging\n");
  struct clock_state ci = { 0 };
  if (est_init(&ci, 5000000))
    return 1;
  /* delta +25 creates slot */
  if (est_tick(&ci, 5000025, 1001))
    return 2;
  /* delta -25 (=0) then 60s rolloff closes slot with power_diff=0 → tp=0 */
  if (est_tick(&ci, 5000000, 1060))
    return 3;
  /* flat tick after rolloff: *s.cnt>0, tp=0, would SIGFPE without guard */
  if (est_tick(&ci, 5000000, 1061))
    return 4;
  if (ci.bat_est_sec != -1)
    return 5;
  return 0;
}

static int test_battery_est_state_tx(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Discharging\n");
  struct clock_state ci = { 0 };
  if (est_init(&ci, 4250000))
    return 1;
  /* switch to Charging — same raw, state transition resets last_raw, no sample yet */
  mock_file("/sys/class/power_supply/BAT0/status", "Charging\n");
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4250000\n");
  test_get_cpu_info(&ci, 1060);
  if (ci.bat_charging != 1)
    return 2;
  /* power rises 30000 µAh over 60 s; remaining = 5000000-4280000 = 720000 */
  /* est = 720000*60/30000 = 1440 */
  if (est_tick(&ci, 4280000, 1120))
    return 3;
  if (ci.bat_pct != 85)
    return 4;
  if (ci.bat_est_sec < 1400 || ci.bat_est_sec > 1480)
    return 5;
  return 0;
}

static int test_bat_biased_first(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Discharging\n");
  struct clock_state ci = { 0 };
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4250000\n");
  test_get_cpu_info(&ci, 1000);
  if (ci.keep.bat_biased_next_dchg != 1)
    return 1;
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4240000\n");
  test_get_cpu_info(&ci, 1030);
  if (ci.keep.bat_discharge_count != 1)
    return 2;
  if (!ci.keep.bat_samples_dchg[0].biased)
    return 3;
  if (ci.keep.bat_biased_next_dchg != 0)
    return 4;
  return 0;
}

static int test_bat_biased_rolloff(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Discharging\n");
  struct clock_state ci = { 0 };
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4250000\n");
  test_get_cpu_info(&ci, 1000);
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4240000\n");
  test_get_cpu_info(&ci, 1060);
  if (ci.keep.bat_discharge_count != 1)
    return 1;
  if (!ci.keep.bat_samples_dchg[0].biased)
    return 2;
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4230000\n");
  test_get_cpu_info(&ci, 1120);
  if (ci.keep.bat_discharge_count != 2)
    return 3;
  if (ci.keep.bat_samples_dchg[1].biased)
    return 4;
  return 0;
}

static int test_bat_consolidate(void)
{
  mock_reset();
  mock_glob("/sys/class/power_supply/*/type", (char *[]) { (char *)"/sys/class/power_supply/BAT0/type" }, 1);
  mock_file("/sys/class/power_supply/BAT0/type", "Battery\n");
  mock_file("/sys/class/power_supply/BAT0/charge_full", "5000000\n");
  mock_file("/sys/class/power_supply/BAT0/status", "Discharging\n");
  struct clock_state ci = { 0 };
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4250000\n");
  test_get_cpu_info(&ci, 1000);
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4240000\n");
  test_get_cpu_info(&ci, 1060);
  if (!ci.keep.bat_samples_dchg[0].biased)
    return 1;
  if (ci.keep.bat_unbiased_full_dchg)
    return 2;
  mock_file("/sys/class/power_supply/BAT0/charge_now", "4230000\n");
  test_get_cpu_info(&ci, 1120);
  if (!ci.keep.bat_unbiased_full_dchg)
    return 3;
  if (ci.keep.bat_samples_dchg[0].duration_sec != 0)
    return 4;
  if (ci.keep.bat_samples_dchg[1].biased)
    return 5;
  return 0;
}

static int test_empty_meminfo(void)
{
  mock_reset();
  mock_file("/proc/meminfo", "");
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (ci.mem_pct != 0)
    return 1;
  return 0;
}

static int test_partial_meminfo(void)
{
  mock_reset();
  mock_glob("/sys/devices/system/cpu/cpu*/cpufreq", (char *[]) { (char *)"/sys/devices/system/cpu/cpu0/cpufreq" }, 1);
  mock_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "1500000\n");
  mock_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "3000000\n");
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (ci.freq != 1500)
    return 1;
  if (ci.freq_max != 3000)
    return 2;
  if (ci.temp != -1)
    return 3;
  return 0;
}

static int test_bad_proc_files(void)
{
  mock_reset();
  mock_file("/proc/stat", "cpu  10 20\n");
  mock_file("/proc/meminfo", "MemTotal: 0 kB\n");
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (ci.pct != 0 || ci.uptime_d != 0 || ci.mem_pct != 0)
    return 1;
  return 0;
}

static int test_cpu_partial_line(void)
{
  mock_reset();
  mock_file("/proc/stat", "cpu  1000 200 50 50 0 0 0 0 0 0\n" "cpu0 200 100 50 50 0 0 0 0 0 0\nfrag");
  mock_file("/proc/meminfo", "MemTotal: 16384000 kB\nMemAvailable: 10000000 kB\n");
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (ci.num_cpus != 1)
    return 1;
  return 0;
}

static int test_storage_delta(void)
{
  mock_reset();
  mock_file("/proc/stat", "cpu  0 0 0 0 0 0 0 0 0 0\n");
  mock_file("/proc/meminfo", "MemTotal: 16384000 kB\nMemAvailable: 9216000 kB\n");
  mock_glob("/sys/block/*", (char *[]) { (char *)"/sys/block/sda" }, 1);
  mock_file("/sys/block/sda/stat", "   0    0 10000    0    0     0 20000    0    0    0    0\n");

  struct clock_state ci = { 0 };
  ci.keep.text = 1;
  test_get_cpu_info(&ci, 0);

  mock_file("/sys/block/sda/stat", "   0    0 12000    0    0     0 25000    0    0    0    0\n");
  ci.sto_line[0] = 0;
  test_get_cpu_info(&ci, 0);

  if (strstr(ci.sto_line, "STO") == NULL)
    return 1;
  if (strstr(ci.sto_line, "sda") == NULL)
    return 2;
  return 0;
}

static void setup_storage_mocks(void)
{
  mock_reset();
  mock_file("/proc/stat", "cpu  0 0 0 0 0 0 0 0 0 0\n");
  mock_file("/proc/meminfo", "MemTotal: 16384000 kB\nMemAvailable: 9216000 kB\n");
  mock_glob("/sys/block/*", (char *[]) { (char *)"/sys/block/sda",
            (char *)"/sys/block/nvme0n1"
            }, 2);
  mock_file("/sys/block/sda/stat", "0 0 0 0 0 0 0 0 0 0 0\n");
  mock_file("/sys/block/nvme0n1/stat", "0 0 0 0 0 0 0 0 0 0 0\n");
  mock_file("/sys/block/sda/dev", "8:0\n");
  mock_file("/sys/block/nvme0n1/dev", "259:0\n");
}

static int test_storage_usage_fmt(void)
{
  mock_reset();
  mock_file("/proc/stat", "cpu  0 0 0 0 0 0 0 0 0 0\n");
  mock_file("/proc/meminfo", "MemTotal: 16384000 kB\nMemAvailable: 9216000 kB\n");
  mock_file("/proc/self/mountinfo",
            "27 1 8:0 / / rw,relatime - ext4 /dev/sda rw\n"
            "bad\n"
            "28 1 8:1 /m /m rw,relatime - ext4 /dev/sdb rw\n" "29 1 8:2 /b /b rw,relatime - ext4 /dev/sdc rw\n");
  {
    struct statvfs st = {.f_blocks = 256,.f_bfree = 128,.f_frsize = 4096 };
    mock_statvfs("/", &st);
  }
  {
    struct statvfs st = {.f_blocks = 1ULL << 32,.f_bfree = 1ULL << 31,.f_frsize = 4096 };
    mock_statvfs("/b", &st);
  }
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (!ci.sto_line[0])
    return 1;
  return 0;
}

static int test_storage_usage(void)
{
  setup_storage_mocks();
  mock_glob("/sys/block/nvme0n1/device/hwmon*/temp*_input",
            (char *[]) { (char *)"/sys/block/nvme0n1/device/hwmon1/temp1_input" }, 1);
  mock_file("/sys/block/nvme0n1/device/hwmon1/temp1_input", "45000\n");
  mock_file("/proc/self/mountinfo",
            "19 27 0:4 / /proc rw,nosuid,nodev,noexec,relatime - proc proc rw\n"
            "27 1 259:3 / / rw,relatime - ext4 /dev/nvme0n1p2 rw\n"
            "30 27 0:22 / /home rw,relatime - ext4 /dev/sda2 rw\n"
            "31 27 0:23 / /sys/fs/fuse/connections rw,relatime - fusectl fusectl rw\n"
            "32 27 0:24 / /run/user/1000/ns rw,relatime - nsfs nsfs rw\n"
            "33 27 0:25 / /proc/sys/fs/binfmt_misc rw,relatime - binfmt_misc binfmt_misc rw\n");
  {
    struct statvfs st = {.f_blocks = 2097152,.f_bfree = 1048576,.f_frsize = 4096 };
    mock_statvfs("/", &st);
  }
  {
    struct statvfs st = {.f_blocks = 524288,.f_bfree = 262144,.f_frsize = 4096 };
    mock_statvfs("/home", &st);
  }
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (ci.sto_temp != 45)
    return 1;
  if (strstr(ci.sto_line, "/") == NULL)
    return 2;
  if (strstr(ci.sto_line, "50%") == NULL)
    return 3;
  if (strstr(ci.sto_line, "/home") == NULL)
    return 4;
  return 0;
}

static int test_parse_body_normal(void)
{
  char ip[64];
  int rc = parse_body("HTTP/1.0 200 OK\r\n\r\n1.2.3.4", ip, sizeof ip);
  if (rc != 0)
    return 1;
  if (strcmp(ip, "1.2.3.4") != 0)
    return 2;
  return 0;
}

static int test_parse_body_ipv6(void)
{
  char ip[64];
  int rc = parse_body("HTTP/1.0 200 OK\r\n\r\n2001:db8::1", ip, sizeof ip);
  if (rc != 0)
    return 1;
  if (strcmp(ip, "2001:db8::1") != 0)
    return 2;
  return 0;
}

static int test_parse_body_no_header(void)
{
  char ip[64];
  int rc = parse_body("no header here", ip, sizeof ip);
  if (rc == 0)
    return 1;
  return 0;
}

static int test_parse_body_spaces(void)
{
  char ip[64];
  int rc = parse_body("HTTP/1.0 200 OK\r\n\r\n   10.0.0.1\n", ip, sizeof ip);
  if (rc != 0)
    return 1;
  if (strcmp(ip, "10.0.0.1") != 0)
    return 2;
  return 0;
}

static int test_storage_dedup(void)
{
  setup_storage_mocks();
  mock_file("/proc/self/mountinfo",
            "27 1 259:3 / / rw,relatime - ext4 /dev/nvme0n1p2 rw\n"
            "30 27 259:4 /boot /boot rw,relatime - ext4 /dev/nvme0n1p3 rw\n" "31 27 259:3 /mnt /mnt rw,relatime - \n");
  {
    struct statvfs st = {.f_blocks = 2097152,.f_bfree = 1048576,.f_frsize = 4096 };
    mock_statvfs("/", &st);
    mock_statvfs("/boot", &st);
  }
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (strstr(ci.sto_line, "/") == NULL)
    return 1;
  if (strstr(ci.sto_line, "/boot") == NULL)
    return 2;
  return 0;
}

static int test_storage_dedup_bind(void)
{
  setup_storage_mocks();
  mock_file("/proc/self/mountinfo",
            "27 1 259:3 / / rw,relatime - ext4 /dev/nvme0n1p2 rw\n"
            "30 27 259:3 /mnt /mnt rw,relatime - ext4 /dev/nvme0n1p2 rw\n");
  {
    struct statvfs st = {.f_blocks = 2097152,.f_bfree = 1048576,.f_frsize = 4096 };
    mock_statvfs("/", &st);
    mock_statvfs("/mnt", &st);
  }
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  if (strstr(ci.sto_line, "/") == NULL)
    return 1;
  if (strstr(ci.sto_line, "/mnt") != NULL)
    return 2;
  return 0;
}

static int test_fmt_thr_kb_fractional(void)
{
  char b[16];
  int n = fmt_thr(b, sizeof b, 1537);
  if (n <= 0)
    return 1;
  if (strcmp(b, "1.5K") != 0)
    return 2;
  return 0;
}

static int test_fmt_thr_kb_integer(void)
{
  char b[16];
  int n = fmt_thr(b, sizeof b, 2048);
  if (n <= 0)
    return 1;
  if (strcmp(b, "2.0K") != 0)
    return 2;
  return 0;
}

static int test_fmt_thr_bytes(void)
{
  char b[16];
  int n = fmt_thr(b, sizeof b, 1536);
  if (n <= 0)
    return 1;
  if (strcmp(b, "1536b") != 0)
    return 2;
  return 0;
}

static int test_fmt_thr_zero(void)
{
  char b[16];
  int n = fmt_thr(b, sizeof b, 0);
  if (n <= 0)
    return 1;
  if (strcmp(b, "0b") != 0)
    return 2;
  return 0;
}

static int test_fmt_thr_kb_max(void)
{
  char b[16];
  int n = fmt_thr(b, sizeof b, 1572864);
  if (n <= 0)
    return 1;
  if (strcmp(b, "1536.0K") != 0)
    return 2;
  return 0;
}

static int test_fmt_thr_mb_integer(void)
{
  char b[16];
  int n = fmt_thr(b, sizeof b, 2097152);
  if (n <= 0)
    return 1;
  if (strcmp(b, "2.0M") != 0)
    return 2;
  return 0;
}

static int test_fmt_thr_mb_fractional(void)
{
  char b[16];
  int n = fmt_thr(b, sizeof b, 1572865);
  if (n <= 0)
    return 1;
  if (strcmp(b, "1.5M") != 0)
    return 2;
  return 0;
}

/* Regression test: sto_line stays within bounds after sto_read_throughput */
static int test_sto_line_bounds(void)
{
  setup_storage_mocks();
  mock_glob("/sys/block/nvme0n1/device/hwmon*/temp*_input",
            (char *[]) { (char *)"/sys/block/nvme0n1/device/hwmon1/temp1_input" }, 1);
  mock_file("/sys/block/nvme0n1/device/hwmon1/temp1_input", "45000\n");
  mock_file("/proc/self/mountinfo",
            "19 27 0:4 / /proc rw,nosuid,nodev,noexec,relatime - proc proc rw\n"
            "27 1 259:3 / / rw,relatime - ext4 /dev/nvme0n1p2 rw\n"
            "30 27 0:22 / /home rw,relatime - ext4 /dev/sda2 rw\n");
  {
    struct statvfs st = {.f_blocks = 2097152,.f_bfree = 1048576,.f_frsize = 4096 };
    mock_statvfs("/", &st);
  }
  struct clock_state ci = { 0 };
  test_get_cpu_info(&ci, 0);
  size_t len = strlen(ci.sto_line);
  if (len >= sizeof ci.sto_line)
    return 1;
  if (ci.sto_line[sizeof ci.sto_line - 1] != '\0')
    return 2;
  return 0;
}

static int test_net_throughput(void)
{
  mock_reset();
  mock_file("/proc/stat", "cpu  0 0 0 0 0 0 0 0 0 0\n");
  mock_file("/proc/meminfo", "MemTotal: 16384000 kB\nMemAvailable: 9216000 kB\n");

  struct clock_state ci = { 0 };
  setup_netlink_wired(1048576, 524288);
  test_get_cpu_info(&ci, 0);

  setup_netlink_wired(3048576, 1572864);
  test_get_cpu_info(&ci, 0);

  if (strstr(ci.net_line, "↓1.9M") == NULL)
    return 1;
  if (strstr(ci.net_line, "↑1.0M") == NULL)
    return 2;
  return 0;
}

static int test_ethtool_speed_gset(void)
{
  mock_reset();
  mock_file("/proc/stat", "cpu  0 0 0 0 0 0 0 0 0 0\n");
  mock_file("/proc/meminfo", "MemTotal: 16384000 kB\nMemAvailable: 9216000 kB\n");
  setup_netlink_wired(1048576, 524288);
  mock_set_link_speed("eth0", 0);
  mock_set_gset_speed("eth0", 1000);
  struct widget_ctx wctx;
  widget_setup(&wctx, 0, NULL);
  struct clock_state ci = { 0 };
  ci.keep.widget = wctx;
  get_net_info(&ci);
  if (ci.keep.net.link_mbps[0] != 1000)
    return 1;
  if (strstr(ci.net_line, "1.0G") == NULL)
    return 2;
  return 0;
}

static int test_ethtool_speed_no_glinksettings(void)
{
  mock_reset();
  mock_file("/proc/stat", "cpu  0 0 0 0 0 0 0 0 0 0\n");
  mock_file("/proc/meminfo", "MemTotal: 16384000 kB\nMemAvailable: 9216000 kB\n");
  setup_netlink_wired(1048576, 524288);
  mock_set_gset_speed("eth0", 1000);
  mock_no_glinksettings = 1;
  struct widget_ctx wctx;
  widget_setup(&wctx, 0, NULL);
  struct clock_state ci = { 0 };
  ci.keep.widget = wctx;
  get_net_info(&ci);
  if (ci.keep.net.link_mbps[0] != 1000)
    return 1;
  if (strstr(ci.net_line, "1.0G") == NULL)
    return 2;
  return 0;
}

static int check_spacing(struct clock_state *ci, unsigned long long rx, unsigned long long tx, int exp)
{
  setup_netlink_full(100000, 200000, rx, tx, -46);
  ci->keep.net.needs_route = 1;
  get_net_info(ci);
  const char *w;
  if (ci->keep.text)
    w = strstr(ci->net_line, "-46dBm");
  else
    w = strstr(ci->net_line, "\xe2\x96\x82\xe2\x96\x84\xe2\x96\x86\xe2\x96\x88");
  if (!w)
    return -1;
  const char *sp;
  for (sp = w; sp > ci->net_line && sp[-1] == ' '; sp--) ;
  return (int)(w - sp) == exp ? 0 : -2;
}

static int test_wifi_align(void)
{
  mock_reset();
  mock_file("/proc/stat", "cpu  0 0 0 0 0 0 0 0 0 0\n");
  mock_file("/proc/meminfo", "MemTotal: 16384000 kB\nMemAvailable: 9216000 kB\n");

  struct clock_state ci = { 0 };
  setup_netlink_full(100000, 200000, 100000, 200000, -46);
  get_net_info(&ci);

  if (check_spacing(&ci, 141062, 203072, 2) != 0)
    return 1;

  if (check_spacing(&ci, 182124, 236864, 1) != 0)
    return 2;

  return 0;
}

static int test_wifi_align_text(void)
{
  mock_reset();
  mock_file("/proc/stat", "cpu  0 0 0 0 0 0 0 0 0 0\n");
  mock_file("/proc/meminfo", "MemTotal: 16384000 kB\nMemAvailable: 9216000 kB\n");

  struct clock_state ci = { 0 };
  ci.keep.text = 1;
  setup_netlink_full(100000, 200000, 100000, 200000, -46);
  get_net_info(&ci);

  if (check_spacing(&ci, 141062, 203072, 1) != 0)
    return 1;

  if (check_spacing(&ci, 182124, 236864, 1) != 0)
    return 2;

  return 0;
}

static int test_widget_order(void)
{
  WidgetType wset[WIDGET_COUNT];
  int n = widget_default_order(wset);
  if (n != WIDGET_COUNT)
    return 2;
  if (wset[0] != WIDGET_DATE)
    return 11;
  if (wset[1] != WIDGET_CPU)
    return 12;
  if (wset[2] != WIDGET_MEM)
    return 13;
  if (wset[3] != WIDGET_GPU)
    return 3;
  return 0;
}

static int test_widget_parse(void)
{
  WidgetType wset[WIDGET_COUNT];
  int n = widget_parse_list("CPU,GPU,MEM", wset, WIDGET_COUNT);
  if (n != 3)
    return 4;
  if (wset[0] != WIDGET_CPU)
    return 41;
  if (wset[1] != WIDGET_GPU)
    return 42;
  if (wset[2] != WIDGET_MEM)
    return 43;
  n = widget_parse_list("", wset, WIDGET_COUNT);
  if (n != 0)
    return 5;
  return 0;
}

static int test_widget_setup(void)
{
  WidgetType wset[WIDGET_COUNT] = { 0 };
  struct widget_ctx wctx = { 0 };
  widget_set_active(&wctx, wset, 0);
  if (widget_active(&wctx, WIDGET_CPU))
    return 6;
  widget_setup(&wctx, 1, "NET");
  widget_setup(&wctx, 0, NULL);
  return 0;
}

static int test_ipv6_equal_metric(void)
{
  mock_reset();
  unsigned char done[16], rte1[64], rte2[64], a6[96];
  mk_done_nl(done);
  size_t rlen = sizeof(struct nlmsghdr) + sizeof(struct rtmsg) + 2 * RTA_LENGTH(sizeof(int));
  size_t alen = sizeof(struct nlmsghdr) + sizeof(struct ifaddrmsg) + RTA_LENGTH(sizeof(struct in6_addr));
  mk_route_nl(rte1, AF_INET6, 1, 0);
  mk_route_nl(rte2, AF_INET6, 2, 0);
  mk_addr6_nl(a6, "2001:db8::2345:6789:abcd:ef01", 1);
  mock_set_netlink(99, done, sizeof(struct nlmsghdr), rte1, rlen);
  mock_add_nl_resp(rte2, rlen);
  mock_add_nl_resp(done, sizeof(struct nlmsghdr));
  mock_add_nl_resp(a6, alen);
  mock_add_nl_resp(done, sizeof(struct nlmsghdr));
  mock_set_ifindex(1);
  struct clock_state ci = { 0 };
  rtnl_open(&ci.keep.rtnl);
  pick_primary(&ci.keep.net, &ci.keep.rtnl, 0);
  if (ci.keep.net.count == 0)
    return 1;
  if (ci.keep.net.ifindex[0] != 1)
    return 2;
  get_local_ip6(&ci);
  if (strcmp(ci.local_ip6, "2001:db8::2345:6789:abcd:ef01") != 0)
    return 3;
  return 0;
}

static int test_pick_primary_v4_over_v6(void)
{
  mock_reset();
  unsigned char done[16], rte_v4a[64], rte_v4b[64], rte_v6[64];
  size_t rlen = sizeof(struct nlmsghdr) + sizeof(struct rtmsg) + 2 * RTA_LENGTH(sizeof(int));
  mk_done_nl(done);
  mk_route_nl(rte_v4a, AF_INET, 7, 100);        /* eth0 (idx=7, metric=100) */
  mk_route_nl(rte_v4b, AF_INET, 4, 200);        /* wlan0 (idx=4, metric=200) */
  mk_route_nl(rte_v6, AF_INET6, 5, 1024);       /* tun0 (idx=5, metric=1024) */
  mock_set_ifname(7, "eth0");
  mock_set_ifname(4, "wlan0");
  mock_set_ifname(5, "tun0");
  mock_set_ifindex(7);
  /* v4 dump: two defaults + done; v6 dump: one default + done */
  mock_set_netlink(99, rte_v4a, rlen, rte_v4b, rlen);
  mock_add_nl_resp(done, sizeof(struct nlmsghdr));
  mock_add_nl_resp(rte_v6, rlen);
  mock_add_nl_resp(done, sizeof(struct nlmsghdr));
  struct clock_state ci = { 0 };
  rtnl_open(&ci.keep.rtnl);
  pick_primary(&ci.keep.net, &ci.keep.rtnl, 0);
  if (ci.keep.net.count == 0)
    return 1;
  if (ci.keep.net.ifindex[0] == 5)
    return 2;
  if (ci.keep.net.ifindex[0] != 7)
    return 3;
  return 0;
}

static int test_ipv6_default_route(void)
{
  mock_reset();
  unsigned char done[16], rte[64], a6[96];
  size_t rlen = sizeof(struct nlmsghdr) + sizeof(struct rtmsg) + 2 * RTA_LENGTH(sizeof(int));
  size_t alen = sizeof(struct nlmsghdr) + sizeof(struct ifaddrmsg) + RTA_LENGTH(sizeof(struct in6_addr));
  mk_done_nl(done);
  mk_route_nl(rte, AF_INET6, 1, 0);
  mk_addr6_nl(a6, "2001:db8::1", 1);
  mock_set_netlink(99, done, sizeof(struct nlmsghdr), rte, rlen);
  mock_add_nl_resp(done, sizeof(struct nlmsghdr));
  mock_add_nl_resp(a6, alen);
  mock_add_nl_resp(done, sizeof(struct nlmsghdr));
  mock_set_ifindex(1);
  struct clock_state ci = { 0 };
  rtnl_open(&ci.keep.rtnl);
  pick_primary(&ci.keep.net, &ci.keep.rtnl, 0);
  if (ci.keep.net.count == 0)
    return 1;
  if (ci.keep.net.ifindex[0] != 1)
    return 2;
  get_local_ip6(&ci);
  if (strcmp(ci.local_ip6, "2001:db8::1") != 0)
    return 3;
  return 0;
}

static int test_widgets(void)
{
  int r;
  if ((r = test_widget_order()))
    return r;
  if ((r = test_widget_parse()))
    return r;
  if ((r = test_widget_setup()))
    return r;
  return 0;
}

static int test_wan_backoff_dns_exhaustion(void)
{
  int rc = 0;
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  WidgetType wset[] = { WIDGET_NET };
  widget_set_active(&ci.keep.widget, wset, 1);
  for (int i = 0; i < 3; i++) {
    pthread_mutex_lock(&ci.keep.async.wan4_result.lock);
    ci.keep.async.wan4_result.state = -1;
    ci.keep.async.wan4_result.reason = 0;
    pthread_mutex_unlock(&ci.keep.async.wan4_result.lock);
    poll_async_fetches(&ci, 0);
  }
  if (ci.keep.wan4.try != 3) {
    rc = 3;
    goto done;
  }
  pthread_mutex_lock(&ci.keep.async.wan4_result.lock);
  ci.keep.async.wan4_result.state = -1;
  ci.keep.async.wan4_result.reason = 0;
  pthread_mutex_unlock(&ci.keep.async.wan4_result.lock);
  poll_async_fetches(&ci, 0);
  if (ci.keep.wan4.try != 3) {
    rc = 4;
    goto done;
  }
 done:
  return rc;
}

static int test_wan_backoff_conn_exhaustion(void)
{
  int rc = 0;
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  WidgetType wset[] = { WIDGET_NET };
  widget_set_active(&ci.keep.widget, wset, 1);
  for (int i = 0; i < 3; i++) {
    pthread_mutex_lock(&ci.keep.async.wan4_result.lock);
    ci.keep.async.wan4_result.state = -1;
    ci.keep.async.wan4_result.reason = 'C';
    pthread_mutex_unlock(&ci.keep.async.wan4_result.lock);
    poll_async_fetches(&ci, 0);
  }
  if (ci.keep.wan4.try != 3) {
    rc = 3;
    goto done;
  }
  pthread_mutex_lock(&ci.keep.async.wan4_result.lock);
  ci.keep.async.wan4_result.state = -1;
  ci.keep.async.wan4_result.reason = 'C';
  pthread_mutex_unlock(&ci.keep.async.wan4_result.lock);
  poll_async_fetches(&ci, 0);
  if (ci.keep.wan4.try != 3) {
    rc = 5;
    goto done;
  }
 done:
  return rc;
}

static int test_wan_backoff_http_exhaustion(void)
{
  int rc = 0;
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  WidgetType wset[] = { WIDGET_NET };
  widget_set_active(&ci.keep.widget, wset, 1);
  for (int i = 0; i < 3; i++) {
    pthread_mutex_lock(&ci.keep.async.wan4_result.lock);
    ci.keep.async.wan4_result.state = -1;
    ci.keep.async.wan4_result.reason = 'H';
    pthread_mutex_unlock(&ci.keep.async.wan4_result.lock);
    poll_async_fetches(&ci, 0);
  }
  if (ci.keep.wan4.try != 3) {
    rc = 3;
    goto done;
  }
  pthread_mutex_lock(&ci.keep.async.wan4_result.lock);
  ci.keep.async.wan4_result.state = -1;
  ci.keep.async.wan4_result.reason = 'H';
  pthread_mutex_unlock(&ci.keep.async.wan4_result.lock);
  poll_async_fetches(&ci, 0);
  if (ci.keep.wan4.try != 3) {
    rc = 5;
    goto done;
  }
 done:
  return rc;
}

static int test_wan_backoff_reset_on_success(void)
{
  int rc = 0;
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  WidgetType wset[] = { WIDGET_NET };
  widget_set_active(&ci.keep.widget, wset, 1);
  ci.keep.wan4.try = 2;
  http_result_write(&ci.keep.async.wan4_result, "203.0.113.42", 0);
  poll_async_fetches(&ci, 0);
  if (ci.keep.wan4.try) {
    rc = 3;
    goto done;
  }
  if (strcmp(ci.keep.pub_ip4, "203.0.113.42") != 0) {
    rc = 4;
    goto done;
  }
 done:
  return rc;
}

static struct async_ctx test_ctx;

static int test_wan_backoff_bypass_on_refetch(void)
{
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, 0);
  async_ctx_init(&test_ctx, NULL, NULL, NULL, (1u << WIDGET_NET) | (1u << WIDGET_WEATHER));
  ci.keep.wan4.try = 3;
  if (any_ready(ci.keep.wan4.try, ci.keep.wan4.retry_ts, 0))
    return 1;
  ci.keep.wan4.try = 0;
  if (!any_ready(ci.keep.wan4.try, ci.keep.wan4.retry_ts, 0))
    return 2;
  dns_cancel(wan_dns_slot(&test_ctx, 0));
  dns_cancel(wan_dns_slot(&test_ctx, 1));
  http_result_cancel(&ci.keep.async.wan4_result);
  http_result_cancel(&ci.keep.async.wan6_result);
  pthread_mutex_lock(&ci.keep.async.wan4_result.lock);
  if (ci.keep.async.wan4_result.state != 0 || ci.keep.async.wan4_result.reason != 0)
    return 3;
  pthread_mutex_unlock(&ci.keep.async.wan4_result.lock);
  return 0;
}

static int test_wan_result_success_v4(void)
{
  int rc = 0;
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  WidgetType wset[] = { WIDGET_NET };
  widget_set_active(&ci.keep.widget, wset, 1);
  http_result_write(&ci.keep.async.wan4_result, "203.0.113.42", 0);
  poll_async_fetches(&ci, 0);
  if (strcmp(ci.keep.pub_ip4, "203.0.113.42") != 0) {
    rc = 3;
    goto done;
  }
  if (ci.keep.wan4.try) {
    rc = 4;
    goto done;
  }
 done:
  return rc;
}

static int test_wan_result_success_v6(void)
{
  int rc = 0;
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  WidgetType wset[] = { WIDGET_NET };
  widget_set_active(&ci.keep.widget, wset, 1);
  http_result_write(&ci.keep.async.wan6_result, "2001:db8::1", 0);
  poll_async_fetches(&ci, 0);
  if (strcmp(ci.keep.pub_ip6, "2001:db8::1") != 0) {
    rc = 3;
    goto done;
  }
  if (ci.keep.wan6.try) {
    rc = 4;
    goto done;
  }
 done:
  return rc;
}

static int test_wan_result_dns_exhausted(void)
{
  int rc = 0;
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  WidgetType wset[] = { WIDGET_NET };
  widget_set_active(&ci.keep.widget, wset, 1);
  pthread_mutex_lock(&ci.keep.async.wan4_result.lock);
  ci.keep.async.wan4_result.state = -1;
  ci.keep.async.wan4_result.reason = 0;
  pthread_mutex_unlock(&ci.keep.async.wan4_result.lock);
  poll_async_fetches(&ci, 0);
  if (ci.keep.wan4.try != 1) {
    rc = 3;
    goto done;
  }
 done:
  return rc;
}

static int test_wan_result_conn_fail(void)
{
  int rc = 0;
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  WidgetType wset[] = { WIDGET_NET };
  widget_set_active(&ci.keep.widget, wset, 1);
  pthread_mutex_lock(&ci.keep.async.wan4_result.lock);
  ci.keep.async.wan4_result.state = -1;
  ci.keep.async.wan4_result.reason = 'C';
  pthread_mutex_unlock(&ci.keep.async.wan4_result.lock);
  poll_async_fetches(&ci, 0);
  if (ci.keep.wan4.try != 1) {
    rc = 3;
    goto done;
  }
 done:
  return rc;
}

static int test_wan_result_http_fail(void)
{
  int rc = 0;
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  WidgetType wset[] = { WIDGET_NET };
  widget_set_active(&ci.keep.widget, wset, 1);
  pthread_mutex_lock(&ci.keep.async.wan4_result.lock);
  ci.keep.async.wan4_result.state = -1;
  ci.keep.async.wan4_result.reason = 'H';
  pthread_mutex_unlock(&ci.keep.async.wan4_result.lock);
  poll_async_fetches(&ci, 0);
  if (ci.keep.wan4.try != 1) {
    rc = 3;
    goto done;
  }
 done:
  return rc;
}

static int test_weather_backoff_dns(void)
{
  int rc = 0;
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, 0);
  ci.keep.weather_refresh_sec = 1;
  ci.keep.weather_refresh_ts = 0;
  ci.keep.weather.try = 3;
  ci.keep.weather.retry_ts = (unsigned long long)-1;
  poll_async_fetches(&ci, 0);
  if (ci.keep.weather.try != 3) {
    rc = 3;
    goto done;
  }
 done:
  return rc;
}

static int test_weather_async(void)
{
  int rc = 0;
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_WEATHER));
  WidgetType wset[1] = { WIDGET_WEATHER };
  widget_set_active(&ci.keep.widget, wset, 1);
  http_result_write(&ci.keep.async.weather_result, "25,27,18,800,Sunny", 0);
  poll_async_fetches(&ci, 0);
  int ok = (ci.keep.weather_temp == 25 && ci.keep.weather_code == 800 && ci.keep.weather_valid == 1);
  ci.keep.async.weather_result.state = 0;
  rc = ok ? 0 : 1;
  return rc;
}

static int test_weather_refresh_skips_in_progress(void)
{
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_WEATHER));
  WidgetType wset[1] = { WIDGET_WEATHER };
  widget_set_active(&ci.keep.widget, wset, 1);
  ci.keep.weather_refresh_sec = 1;
  ci.keep.weather_refresh_ts = 0;
  ci.keep.weather_state = WAN_DNS;

  /* Refresh timer expired but fetch in progress — should NOT restart */
  poll_async_fetches(&ci, 10);
  if (ci.keep.weather_state != WAN_DNS)
    return 1;

  /* Now idle; refresh should proceed */
  ci.keep.weather_state = WAN_IDLE;
  ci.keep.weather_refresh_ts = 0;
  poll_async_fetches(&ci, 20);
  if (ci.keep.weather_state != WAN_DNS)
    return 2;

  return 0;
}

static int test_dns_slot_idle(void)
{
  struct dns_slot slot = {.lock = PTHREAD_MUTEX_INITIALIZER };
  struct sockaddr_storage ss;
  return dns_read_slot(&slot, &ss, sizeof ss) == 0 ? 0 : 1;
}

static int test_dns_slot_error(void)
{
  struct dns_slot slot = {.lock = PTHREAD_MUTEX_INITIALIZER };
  struct sockaddr_storage ss;
  slot.state = -1;
  return dns_read_slot(&slot, &ss, sizeof ss) == -1 ? 0 : 1;
}

static struct widget_ctx test_wctx;

static int test_weather_dns_short_addr(void)
{
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, test_wctx.active_mask);
  ci.keep.widget = test_wctx;
  http_result_write(&ci.keep.async.weather_result, NULL, 'C');
  poll_async_fetches(&ci, 0);
  pthread_mutex_lock(&ci.keep.async.weather_result.lock);
  int s = ci.keep.async.weather_result.state;
  pthread_mutex_unlock(&ci.keep.async.weather_result.lock);
  return s == 0 ? 0 : 1;
}

static void *idle_thread(void *arg)
{
  (void)arg;
  struct timespec ts = {.tv_sec = 99999 };
  nanosleep(&ts, NULL);
  return NULL;
}

static int test_dns_cancel(void)
{
  struct dns_slot slot = {.lock = PTHREAD_MUTEX_INITIALIZER };
  pthread_t th;
  pthread_create(&th, NULL, idle_thread, NULL);
  pthread_mutex_lock(&slot.lock);
  slot.state = 1;
  slot.thread = th;
  pthread_mutex_unlock(&slot.lock);
  dns_cancel(&slot);
  pthread_mutex_lock(&slot.lock);
  int s = slot.state;
  int c = slot.cancelled;
  pthread_mutex_unlock(&slot.lock);
  return (s == 0 && c == 1) ? 0 : 1;
}

static int test_dns_cancel_idle(void)
{
  struct dns_slot slot = {.lock = PTHREAD_MUTEX_INITIALIZER };
  dns_cancel(&slot);
  pthread_mutex_lock(&slot.lock);
  int s = slot.state;
  int c = slot.cancelled;
  pthread_mutex_unlock(&slot.lock);
  return (s == 0 && c == 0) ? 0 : 1;
}

static int test_wan_dns_cancel_race(void)
{
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  struct dns_slot *slot = wan_dns_slot(&ci.keep.async, 0);
  wan_dns_start(&ci, 0);
  dns_cancel(slot);
  pthread_mutex_lock(&slot->lock);
  int s = slot->state;
  int c = slot->cancelled;
  pthread_t th = slot->thread;
  pthread_mutex_unlock(&slot->lock);
  if (!c)
    pthread_join(th, NULL);
  return (s == 0 || s == -1) ? 0 : 1;
}

/* --- ioserv tests --- */

static int http_result_read(struct http_result *slot, char *out, int maxlen)
{
  pthread_mutex_lock(&slot->lock);
  int s = slot->state;
  if (s == 2) {
    int n = strlen(slot->data);
    if (n >= maxlen)
      n = maxlen - 1;
    memcpy(out, slot->data, n);
    out[n] = 0;
    slot->state = 0;
    slot->cancelled = 0;
  }
  pthread_mutex_unlock(&slot->lock);
  return s == 2 ? 0 : -1;
}

static int test_http_result_write_read(void)
{
  struct http_result hr = {.lock = PTHREAD_MUTEX_INITIALIZER };
  http_result_write(&hr, "1.2.3.4", 0);
  if (hr.state != 2 || hr.reason != 0)
    return 1;
  char buf[48] = { 0 };
  if (http_result_read(&hr, buf, sizeof buf) != 0)
    return 2;
  if (strcmp(buf, "1.2.3.4") != 0)
    return 3;
  return hr.state == 0 ? 0 : 4;
}

static int test_http_result_write_error(void)
{
  struct http_result hr = {.lock = PTHREAD_MUTEX_INITIALIZER };
  http_result_write(&hr, NULL, 'C');
  if (hr.state != -1 || hr.reason != 'C')
    return 1;
  char buf[48];
  return http_result_read(&hr, buf, sizeof buf) == -1 ? 0 : 2;
}

static int test_http_result_cancel(void)
{
  struct http_result hr = {.lock = PTHREAD_MUTEX_INITIALIZER };
  http_result_cancel(&hr);
  if (!hr.cancelled)
    return 1;
  http_result_write(&hr, "1.2.3.4", 0);
  if (hr.state != 0)
    return 2;
  return hr.data[0] == 0 ? 0 : 3;
}

static int test_http_result_read_truncated(void)
{
  struct http_result hr = {.lock = PTHREAD_MUTEX_INITIALIZER };
  http_result_write(&hr, "1234567890", 0);
  if (hr.state != 2)
    return 1;
  char buf[5] = { 0 };
  if (http_result_read(&hr, buf, 5) != 0)
    return 2;
  if (strcmp(buf, "1234") != 0)
    return 3;
  return 0;
}

static int test_io_init(void)
{
  struct ioserv_ctl ctl = { 0 };
  if (io_init(&ctl) != 0)
    return 1;
  if (ctl.efd != -1 || ctl.io_thread_active != 0)
    return 2;
  if (ctl.shutdown != 0 || ctl.cancel_all != 0)
    return 3;
  return ctl.pending_work == 0 ? 0 : 4;
}

static int test_io_cancel_all(void)
{
  struct ioserv_ctl ctl = { 0 };
  io_init(&ctl);
  ctl.efd = eventfd(0, EFD_NONBLOCK);
  if (ctl.efd < 0)
    return 1;
  io_cancel_all(&ctl, 0);
  if (!ctl.cancel_all)
    return 2;
  if (ctl.weather_cancel)
    return 3;
  if (ctl.efd < 0)
    return 4;
  io_init(&ctl);
  ctl.efd = eventfd(0, EFD_NONBLOCK);
  if (ctl.efd < 0)
    return 5;
  io_cancel_all(&ctl, 2);
  if (!ctl.weather_cancel)
    return 6;
  if (ctl.cancel_all)
    return 7;
  close(ctl.efd);
  return 0;
}

static int test_io_shutdown(void)
{
  struct ioserv_ctl ctl = { 0 };
  io_init(&ctl);
  ctl.efd = eventfd(0, EFD_NONBLOCK);
  if (ctl.efd < 0)
    return 1;
  io_shutdown(&ctl);
  if (!ctl.shutdown)
    return 2;
  close(ctl.efd);
  return 0;
}

/* extern declarations for still-public ioserv functions */
extern void io_close_conn(struct io_conn *c);
extern int io_setup_conns(struct async_ctx *ctx, struct io_conn *conns);

static int test_io_close_conn(void)
{
  struct io_conn c = {.fd = -1 };
  io_close_conn(&c);
  if (c.phase != 0 || c.fd != -1)
    return 1;
  int p[2];
  if (pipe(p) < 0)
    return 2;
  c.fd = p[0];
  c.phase = 3;
  io_close_conn(&c);
  if (c.fd != -1 || c.phase != 0)
    return 3;
  if (c.req_sent != 0 || c.resp_len != 0)
    return 4;
  close(p[1]);
  return 0;
}

static int test_io_setup_conns(void)
{
  struct io_conn conns[3] = { 0 };
  io_setup_conns(&test_ctx, conns);
  if (!conns[0].dns || !conns[0].result || !conns[0].parse)
    return 1;
  if (!conns[1].dns || !conns[1].result || !conns[1].parse)
    return 2;
  if (!conns[2].dns || !conns[2].result || !conns[2].parse)
    return 3;
  return 0;
}

static int test_io_thread_run_shutdown(void)
{
  struct ioserv_ctl ctl = { 0 };
  io_init(&ctl);
  ctl.shutdown = 1;
  pthread_t th;
  if (pthread_create(&th, NULL, io_thread_run, &ctl) != 0)
    return 1;
  pthread_join(th, NULL);
  if (ctl.io_thread_active)
    return 2;
  return 0;
}

static int test_once_3s_deadline(void)
{
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  ci.keep.net.count = 0;
  strncpy(ci.keep.ipv4_local, "192.168.1.1", 16);
  ci.keep.ipv6_local[0] = 0;
  io_init(&ci.keep.async.ioc);
  WidgetType wset[] = { WIDGET_NET };
  widget_set_active(&ci.keep.widget, wset, 1);
  unsigned long long t0 = (unsigned long long)time(0);
  once_wait(&ci, t0);
  unsigned long long dt = (unsigned long long)time(0) - t0;
  if (dt > 4)
    return 1;
  if (ci.keep.pub_ip4[0] || ci.keep.pub_ip6[0])
    return 2;
  return 0;
}

static int test_once_3s_ok(void)
{
  struct clock_state ci = { 0 };
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  ci.keep.net.count = 0;
  strncpy(ci.keep.ipv4_local, "192.168.1.1", 16);
  ci.keep.ipv6_local[0] = 0;
  io_init(&ci.keep.async.ioc);
  WidgetType wset[] = { WIDGET_NET };
  widget_set_active(&ci.keep.widget, wset, 1);
  pthread_mutex_lock(&ci.keep.async.wan4_result.lock);
  ci.keep.async.wan4_result.cancelled = 0;
  strncpy(ci.keep.async.wan4_result.data, "203.0.113.42", sizeof ci.keep.async.wan4_result.data - 1);
  ci.keep.async.wan4_result.data[sizeof ci.keep.async.wan4_result.data - 1] = 0;
  ci.keep.async.wan4_result.state = 2;
  ci.keep.async.wan4_result.reason = 0;
  pthread_mutex_unlock(&ci.keep.async.wan4_result.lock);
  pthread_mutex_lock(&ci.keep.async.wan6_result.lock);
  ci.keep.async.wan6_result.state = HTTP_ERROR;
  ci.keep.async.wan6_result.cancelled = 0;
  pthread_mutex_unlock(&ci.keep.async.wan6_result.lock);
  unsigned long long t0 = (unsigned long long)time(0);
  once_wait(&ci, t0);
  unsigned long long dt = (unsigned long long)time(0) - t0;
  if (dt > 1)
    return 1;
  if (strcmp(ci.keep.pub_ip4, "203.0.113.42") != 0)
    return 2;
  return 0;
}

static void test_get_cpu_info(struct clock_state *ci, time_t now)
{
  ci->keep.widget = test_wctx;
  ci->keep.net.needs_route = 1;
  gather_all(ci, now);
}

static int test_io_thread_run_normal(void)
{
  struct async_ctx ctx = { 0 };
  int fd4 = -1, fd6 = -1, wfd = -1;
  async_ctx_init(&ctx, &fd4, &fd6, &wfd, 0);
  struct ioserv_ctl ctl;
  io_init(&ctl);
  ctl.owner = &ctx;
  pthread_t th;
  if (pthread_create(&th, NULL, io_thread_run, &ctl) != 0)
    return 1;
  pthread_join(th, NULL);
  if (ctl.io_thread_active)
    return 2;
  return 0;
}

static int test_io_thread_run_loop(void)
{
  struct async_ctx ctx = { 0 };
  int fd4 = -1, fd6 = -1, wfd = -1;
  async_ctx_init(&ctx, &fd4, &fd6, &wfd, (1u << WIDGET_NET) | (1u << WIDGET_WEATHER));
  struct dns_slot *s4 = wan_dns_slot(&ctx, 0);
  pthread_mutex_lock(&s4->lock);
  s4->state = DNS_RUNNING;
  pthread_mutex_unlock(&s4->lock);
  struct dns_slot *s6 = wan_dns_slot(&ctx, 1);
  pthread_mutex_lock(&s6->lock);
  s6->state = DNS_RUNNING;
  pthread_mutex_unlock(&s6->lock);
  struct ioserv_ctl ctl;
  io_init(&ctl);
  ctl.owner = &ctx;
  pthread_t th;
  if (pthread_create(&th, NULL, io_thread_run, &ctl) != 0)
    return 1;
  usleep(50000);
  {
    uint64_t v = 1;
    int efd = ctl.efd;
    if (efd >= 0)
      write(efd, &v, sizeof v);
  }
  io_shutdown(&ctl);
  pthread_join(th, NULL);
  return ctl.io_thread_active ? 2 : 0;
}

static int test_dns_start_real(void)
{
  struct async_ctx ctx = { 0 };
  int fd4 = -1, fd6 = -1, wfd = -1;
  async_ctx_init(&ctx, &fd4, &fd6, &wfd, (1u << WIDGET_NET));
  mock_syscall_real_threads = 1;
  dns_start(&ctx, &ctx.wan4_dns, "127.0.0.1", AF_INET);
  for (int i = 0; i < 500; i++) {
    pthread_mutex_lock(&ctx.wan4_dns.lock);
    int done = ctx.wan4_dns.state == DNS_DONE || ctx.wan4_dns.state == DNS_ERROR;
    pthread_mutex_unlock(&ctx.wan4_dns.lock);
    if (done)
      break;
    usleep(1000);
  }
  dns_cancel(&ctx.wan4_dns);
  int ok = 0;
  pthread_mutex_lock(&ctx.wan4_dns.lock);
  if (ctx.wan4_dns.state == DNS_DONE || ctx.wan4_dns.state == DNS_IDLE)
    ok = 1;
  pthread_mutex_unlock(&ctx.wan4_dns.lock);
  mock_syscall_real_threads = 0;
  for (int i = 0; i < 500; i++) {
    pthread_mutex_lock(&ctx.ioc.lock);
    int active = ctx.ioc.io_thread_active;
    pthread_mutex_unlock(&ctx.ioc.lock);
    if (!active)
      break;
    usleep(10000);
  }
  return ok ? 0 : 1;
}

static int test_weather_parse_json(void)
{
  char buf[256] = { 0 };
  const char *resp =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/json\r\n"
      "\r\n"
      "{\"current_condition\":[{\"FeelsLikeC\":\"22\",\"humidity\":\"50\",\"localObsDateTime\":\"2024-01-01 12:00\",\"temp_C\":\"25\",\"weatherCode\":\"113\",\"weatherDesc\":[{\"value\":\"Sunny\"}]}],\"weather\":[{\"maxtempC\":\"28\",\"mintempC\":\"18\"}]}";
  int r = weather_parse_resp(resp, buf, sizeof buf);
  if (r != 0)
    return 1;
  char expected[256];
  snprintf(expected, sizeof expected, "25,28,18,113,Sunny");
  if (strcmp(buf, expected) != 0)
    return 2;
  /* multi-word description */
  const char *resp2 =
      "HTTP/1.1 200 OK\r\n"
      "\r\n"
      "{\"current_condition\":[{\"temp_C\":\"22\",\"weatherCode\":\"119\",\"weatherDesc\":[{\"value\":\"Partly cloudy\"}]}],\"weather\":[{\"maxtempC\":\"25\",\"mintempC\":\"15\"}]}";
  r = weather_parse_resp(resp2, buf, sizeof buf);
  if (r != 0)
    return 4;
  snprintf(expected, sizeof expected, "22,25,15,119,Partly cloudy");
  if (strcmp(buf, expected) != 0)
    return 5;
  const char *e = weather_emoji(113);
  if (!e || strlen(e) == 0)
    return 3;
  return 0;
}

static int test_weather_dns_start_call(void)
{
  struct clock_state ci = { 0 };
  int fd4 = -1, fd6 = -1, wfd = -1;
  async_ctx_init(&ci.keep.async, &fd4, &fd6, &wfd, 0);
  weather_dns_start(&ci);
  dns_cancel(&ci.keep.async.weather_dns);
  return ci.keep.weather_state == WAN_DNS ? 0 : 1;
}

static int test_io_thread_run_with_shutdown(void)
{
  struct async_ctx ctx = { 0 };
  int fd4 = -1, fd6 = -1, wfd = -1;
  async_ctx_init(&ctx, &fd4, &fd6, &wfd, 0);
  struct ioserv_ctl ctl;
  io_init(&ctl);
  ctl.owner = &ctx;
  struct dns_slot *ds = wan_dns_slot(&ctx, 0);
  pthread_mutex_lock(&ds->lock);
  ds->state = 1;
  pthread_mutex_unlock(&ds->lock);
  pthread_t th;
  if (pthread_create(&th, NULL, io_thread_run, &ctl) != 0)
    return 1;
  usleep(50000);
  io_shutdown(&ctl);
  pthread_join(th, NULL);
  if (ctl.io_thread_active)
    return 2;
  return 0;
}

static int test_disk_grow_guard(void)
{
  struct disk_ctx d;
  memset(&d, 0, sizeof d);
  char path[128], content[16];
  mock_reset();
  for (int i = 0; i < 10; i++) {
    snprintf(path, sizeof path, "/sys/block/sd%c/dev", 'a' + i);
    snprintf(content, sizeof content, "%d:0\n", 8 + i);
    mock_file(path, content);
    snprintf(path, sizeof path, "/sys/block/sd%c/size", 'a' + i);
    mock_file(path, "12345678\n");
  }
  for (int i = 0; i < 10; i++) {
    unsigned idx = 99;
    snprintf(path, sizeof path, "/sys/block/sd%c", 'a' + i);
    int v = discover_dev(&d, path, &idx);
    if (v < 0 || v > 1)
      return 1;
    if (idx != (unsigned)i)
      return 2;
  }
  if (d.cnt != 10)
    return 3;
  if (d.cap > 64)
    return 4;
  free(d.devs);
  return 0;
}

static int test_cpu_temp_sentinel(void)
{
  struct clock_state ci = { 0 };
  ci.keep.text = 1;
  ci.pct = 10;
  ci.iowait_pct = 5;
  ci.load = 1.0;
  ci.num_cpus = 1;
  ci.freq = 2200000;
  ci.freq_max = 3700000;
  snprintf(ci.governor, sizeof ci.governor, "%s", "powersave");
  WidgetType wset[] = { WIDGET_CPU };
  widget_set_active(&ci.keep.widget, wset, 1);

  /* temp = -1 (unavailable) must not render "-1" */
  ci.temp = -1;
  char buf[RENDER_BUF];
  int n = fmt_widget_list(buf, sizeof buf, &ci, "");
  buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = 0;
  if (strstr(buf, "-1"))
    return 1;
  if (!strstr(buf, "CPU"))
    return 2;

  /* temp = 50 (valid) must render "50°C" */
  ci.temp = 50;
  n = fmt_widget_list(buf, sizeof buf, &ci, "");
  buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = 0;
  if (!strstr(buf, "50\xc2\xb0" "C"))
    return 3;

  /* temp = 0 also not rendered (same as -1) */
  ci.temp = 0;
  n = fmt_widget_list(buf, sizeof buf, &ci, "");
  buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = 0;
  if (strstr(buf, "0\xc2\xb0" "C"))
    return 4;

  return 0;
}

int main(void)
{
  widget_setup(&test_wctx, 0, NULL);
  static const struct {
    const char *name;
    int (*fn)(void);
    int off;
  } run[] = {
    {"test_widgets", test_widgets, 0},
    {"test_normal", test_normal, 0},
    {"test_cached_paths", test_cached_paths, 20},
    {"test_fan_stale_newline", test_fan_stale_newline, 30},
    {"test_mobo_fan", test_mobo_fan, 50},
    {"test_missing_files", test_missing_files, 100},
    {"test_container_unlimited", test_container_unlimited, 120},
    {"test_cpu_delta", test_cpu_delta, 200},
    {"test_cpu_temp_sentinel", test_cpu_temp_sentinel, 0},
    {"test_battery_energy_now", test_battery_energy_now, 250},
    {"test_battery_charging", test_battery_charging, 300},
    {"test_battery_full", test_battery_full, 400},
    {"test_battery_not_charging", test_battery_not_charging, 450},
    {"test_battery_recalibrate_full", test_battery_recalibrate_full, 460},
    {"test_no_battery", test_no_battery, 500},
    {"test_battery_est_discharge", test_battery_est_discharge, 510},
    {"test_battery_est_charge", test_battery_est_charge, 520},
    {"test_battery_est_flat", test_battery_est_flat, 530},
    {"test_battery_est_zero_power", test_battery_est_zero_power, 535},
    {"test_battery_est_state_tx", test_battery_est_state_tx, 540},
    {"test_bat_biased_first", test_bat_biased_first, 545},
    {"test_bat_biased_rolloff", test_bat_biased_rolloff, 550},
    {"test_bat_consolidate", test_bat_consolidate, 555},
    {"test_empty_meminfo", test_empty_meminfo, 600},
    {"test_partial_meminfo", test_partial_meminfo, 700},
    {"test_bad_proc_files", test_bad_proc_files, 800},
    {"test_cpu_partial_line", test_cpu_partial_line, 900},
    {"test_disk_grow_guard", test_disk_grow_guard, 950},
    {"test_storage_delta", test_storage_delta, 1000},
    {"test_storage_usage_fmt", test_storage_usage_fmt, 100},
    {"test_storage_usage", test_storage_usage, 1100},
    {"test_storage_dedup", test_storage_dedup, 1200},
    {"test_storage_dedup_bind", test_storage_dedup_bind, 1200},
    {"test_fmt_thr_kb_fractional", test_fmt_thr_kb_fractional, 1300},
    {"test_fmt_thr_kb_integer", test_fmt_thr_kb_integer, 1301},
    {"test_fmt_thr_bytes", test_fmt_thr_bytes, 1302},
    {"test_fmt_thr_mb_fractional", test_fmt_thr_mb_fractional, 1303},
    {"test_fmt_thr_zero", test_fmt_thr_zero, 1304},
    {"test_fmt_thr_kb_max", test_fmt_thr_kb_max, 1305},
    {"test_fmt_thr_mb_integer", test_fmt_thr_mb_integer, 1306},
    {"test_sto_line_bounds", test_sto_line_bounds, 1307},
    {"test_parse_body_normal", test_parse_body_normal, 1307},
    {"test_parse_body_ipv6", test_parse_body_ipv6, 1400},
    {"test_parse_body_no_header", test_parse_body_no_header, 1500},
    {"test_parse_body_spaces", test_parse_body_spaces, 1600},
    {"test_net_throughput", test_net_throughput, 1700},
    {"test_ethtool_speed_gset", test_ethtool_speed_gset, 1750},
    {"test_ethtool_speed_no_glinksettings", test_ethtool_speed_no_glinksettings, 1760},
    {"test_wifi_align", test_wifi_align, 1800},
    {"test_wifi_align_text", test_wifi_align_text, 1900},
    {"test_ipv6_default_route", test_ipv6_default_route, 2000},
    {"test_pick_primary_v4_over_v6", test_pick_primary_v4_over_v6, 2050},
    {"test_ipv6_equal_metric", test_ipv6_equal_metric, 2100},
    {"test_wan_result_success_v4", test_wan_result_success_v4, 2200},
    {"test_wan_result_success_v6", test_wan_result_success_v6, 2300},
    {"test_wan_result_dns_exhausted", test_wan_result_dns_exhausted, 2400},
    {"test_wan_result_conn_fail", test_wan_result_conn_fail, 2450},
    {"test_wan_result_http_fail", test_wan_result_http_fail, 2460},
    {"test_wan_backoff_dns_exhaustion", test_wan_backoff_dns_exhaustion, 2465},
    {"test_wan_backoff_conn_exhaustion", test_wan_backoff_conn_exhaustion, 2470},
    {"test_wan_backoff_http_exhaustion", test_wan_backoff_http_exhaustion, 2475},
    {"test_wan_backoff_reset_on_success", test_wan_backoff_reset_on_success, 2480},
    {"test_wan_backoff_bypass_on_refetch", test_wan_backoff_bypass_on_refetch, 2485},
    {"test_weather_backoff_dns", test_weather_backoff_dns, 2500},
    {"test_weather_async", test_weather_async, 2500},
    {"test_weather_refresh_skips_in_progress", test_weather_refresh_skips_in_progress, 2505},
    {"test_dns_slot_idle", test_dns_slot_idle, 2600},
    {"test_dns_slot_error", test_dns_slot_error, 2700},
    {"test_weather_dns_short_addr", test_weather_dns_short_addr, 2980},
    {"test_dns_cancel", test_dns_cancel, 3100},
    {"test_dns_cancel_idle", test_dns_cancel_idle, 3150},
    {"test_wan_dns_cancel_race", test_wan_dns_cancel_race, 3200},
    {"test_http_result_write_read", test_http_result_write_read, 3320},
    {"test_http_result_write_error", test_http_result_write_error, 3330},
    {"test_http_result_cancel", test_http_result_cancel, 3340},
    {"test_http_result_read_truncated", test_http_result_read_truncated, 3345},
    {"test_io_init", test_io_init, 3350},
    {"test_io_cancel_all", test_io_cancel_all, 3360},
    {"test_io_shutdown", test_io_shutdown, 3370},
    {"test_io_close_conn", test_io_close_conn, 3380},
    {"test_io_setup_conns", test_io_setup_conns, 3450},
    {"test_io_thread_run_shutdown", test_io_thread_run_shutdown, 3520},
    {"test_io_thread_run_normal", test_io_thread_run_normal, 3530},
    {"test_io_thread_run_with_shutdown", test_io_thread_run_with_shutdown, 3540},
    {"test_io_thread_run_loop", test_io_thread_run_loop, 3545},
    {"test_dns_start_real", test_dns_start_real, 3550},
    {"test_weather_parse_json", test_weather_parse_json, 3552},
    {"test_weather_dns_start_call", test_weather_dns_start_call, 3555},
    {"test_once_3s_deadline", test_once_3s_deadline, 3560},
    {"test_once_3s_ok", test_once_3s_ok, 3570},
  };
  for (size_t i = 0; i < sizeof run / sizeof *run; i++) {
    printf("%s\n", run[i].name);
    int rc = run[i].fn();
    if (rc)
      return run[i].off + rc;
  }
  return 0;
}
