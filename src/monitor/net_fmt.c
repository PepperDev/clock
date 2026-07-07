#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <sys/ioctl.h>          // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <net/if.h>             // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include <linux/ethtool.h>      // cppcheck-suppress missingIncludeSystem
#include <linux/rtnetlink.h>    // cppcheck-suppress missingIncludeSystem
#ifndef SIOCETHTOOL
#define SIOCETHTOOL 0x8946
#endif
#ifndef ETHTOOL_GSET
#define ETHTOOL_GSET 0x00000001
#endif
#include <stdarg.h>             // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

enum { RATE_LN_SZ = 24, SPEED_LN_SZ = 48, WLAN_LN_SZ = 96 };

static int ethtool_ioctl(int fd, const char *iface, void *data)
{
  struct ifreq ifr;
  memset(&ifr, 0, sizeof ifr);
  memcpy(ifr.ifr_name, iface, sizeof ifr.ifr_name - 1);
  ifr.ifr_name[sizeof ifr.ifr_name - 1] = 0;
  ifr.ifr_data = data;
  return sys_ioctl(fd, SIOCETHTOOL, &ifr);
}

static int ethtool_need_nwords(int fd, const char *iface)
{
  struct ethtool_link_settings s;
  memset(&s, 0, sizeof s);
  s.cmd = ETHTOOL_GLINKSETTINGS;
  if (ethtool_ioctl(fd, iface, &s) != 0)
    return -1;
  return s.link_mode_masks_nwords > 0 ? s.link_mode_masks_nwords : 0;
}

static int ethtool_speed(int fd, const char *iface, int nw)
{
  unsigned char buf[sizeof(struct ethtool_link_settings) + 3 * (__ETHTOOL_LINK_MODE_MASK_NBITS / 32) * sizeof(__u32)];
  struct ethtool_link_settings *p = (struct ethtool_link_settings *)buf;
  memset(p, 0, sizeof buf);
  p->cmd = ETHTOOL_GLINKSETTINGS;
  p->link_mode_masks_nwords = (__s8) nw;
  if (ethtool_ioctl(fd, iface, p) != 0)
    return 0;
  return p->speed > 0 ? (int)p->speed : 0;
}

static int read_speed_gset(int fd, const char *iface)
{
  struct ethtool_cmd ecmd;
  memset(&ecmd, 0, sizeof ecmd);
  ecmd.cmd = ETHTOOL_GSET;
  if (ethtool_ioctl(fd, iface, &ecmd) != 0)
    return 0;
  int speed = ecmd.speed_hi ? (((unsigned)ecmd.speed_hi << 16) | ecmd.speed) : ecmd.speed;
  return speed > 0 ? speed : 0;
}

static int resolve_iface_speed(struct rtnl_ctx *r, int fd, int idx)
{
  if (idx < 0)
    return 0;
  struct link_entry *e = &r->links[idx];
  if (e->speed >= 0)
    return e->speed;
  const char *iface = e->name;
  int nw = ethtool_need_nwords(fd, iface);
  int spd = 0;
  if (nw >= 0)
    spd = ethtool_speed(fd, iface, nw);
  if (spd <= 0)
    spd = read_speed_gset(fd, iface);
  e->speed = spd > 0 ? spd : 0;
  return e->speed;
}

static void read_link_speed(struct rtnl_ctx *r, struct net_ctx *c, int fd)
{
  for (unsigned i = 0; i < c->count; i++) {
    if (i == (unsigned)c->wlan_idx)
      continue;
    c->link_mbps[i] = resolve_iface_speed(r, fd, c->link_idx[i]);
  }
}

#define FMT_THR_BYTE_MAX 512
#define FMT_THR_UNIT 100

// cppcheck-suppress staticFunction - used by tests
int net_fmt_thr(char *b, int z, unsigned long long v)
{
  if (v < FMT_THR_BYTE_MAX)
    return snprintf(b, z, "%llub", v);
  if (v < FMT_THR_UNIT * BYTES_PER_KB)
    return snprintf(b, z, "%.1fK", (double)(v * 10ULL / BYTES_PER_KB) / 10);
  if (v < FMT_THR_UNIT * BYTES_PER_MB)
    return snprintf(b, z, "%.1fM", (double)(v * 10ULL / BYTES_PER_MB) / 10);
  return snprintf(b, z, "%.1fG", (double)(v * 10ULL / BYTES_PER_GB) / 10);
}

static int read_dbm(const struct net_ctx *c)
{
  return c->wlan_dbm;
}

#define WLAN_G_MBPS 1000
#define WLAN_G_BOTH_MBPS 500
#define MBPS_PER_GBPS 1000
#define WIFI_THR_WIDTH 11
#define WIFI_PAD_MAX 7

static int wifi_pad(int lr, int lt)
{
  int p = WIFI_THR_WIDTH - lr - lt;
  return p < 1 ? 1 : p > WIFI_PAD_MAX ? WIFI_PAD_MAX : p;
}

static int wlan_g_rate(int r, int t)
{
  if (r >= WLAN_G_MBPS)
    return 1;
  if (t >= WLAN_G_MBPS)
    return 1;
  if (r >= WLAN_G_BOTH_MBPS && t >= WLAN_G_BOTH_MBPS)
    return 1;
  return 0;
}

static void fmt_rate_str(char *s, int sz, int r, int t)
{
  if (r > 0) {
    if (t > 0) {
      if (wlan_g_rate(r, t))
        snprintf(s, sz, " %.1f/%.1fG", r / MHZ_PER_GHZ, t / MHZ_PER_GHZ);
      else
        snprintf(s, sz, " %d/%dM", r, t);
    } else {
      if (wlan_g_rate(r, 0))
        snprintf(s, sz, " %.1fG", r / MHZ_PER_GHZ);
      else
        snprintf(s, sz, " %dM", r);
    }
  } else {
    if (wlan_g_rate(0, t))
      snprintf(s, sz, " %.1fG", t / MHZ_PER_GHZ);
    else
      snprintf(s, sz, " %dM", t);
  }
}

static void iface_speed_str(const struct clock_state *ci, int i, char *s, int sz)
{
  if (ci->keep.net.link_mbps[i] > 0) {
    int v = ci->keep.net.link_mbps[i];
    if (v >= MBPS_PER_GBPS)
      snprintf(s, sz, " %.1fG", v / MHZ_PER_GHZ);
    else
      snprintf(s, sz, " %dM", v);
  }
}

static size_t net_line_rem(const struct clock_state *ci, char **pp)
{
  return (ci->net_line + sizeof ci->net_line) - *pp;
}

static void fmt_into(char **pp, size_t rem, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(*pp, rem, fmt, ap);
  va_end(ap);
  if (n > 0 && rem > 0) {
    if ((size_t)n >= rem)
      n = (int)rem - 1;
    *pp += n;
  }
}

static void fmt_iface_line(const struct clock_state *ci, int i, unsigned long long rx, unsigned long long tx, char **pp)
{
  char r[RATE_SZ], t[RATE_SZ], s[SPEED_LN_SZ] = "";
  net_fmt_thr(r, sizeof r, rx);
  net_fmt_thr(t, sizeof t, tx);
  iface_speed_str(ci, i, s, sizeof s);
  const char *name = name_idx_by_idx((struct rtnl_ctx *)&ci->keep.rtnl, (unsigned)ci->keep.net.ifindex[i]);
  if (!name)
    name = "";
  fmt_into(pp, net_line_rem(ci, pp), "%s \xe2\x86\x93%s\xe2\x86\x91%s%s", name, r, t, s);
}

static const char *fmt_bars(int dbm)
{
  if (dbm >= -50)
    return "\xe2\x96\x82\xe2\x96\x84\xe2\x96\x86\xe2\x96\x88 ";
  if (dbm >= -60)
    return "\xe2\x96\x82\xe2\x96\x84\xe2\x96\x86 ";
  if (dbm >= -70)
    return "\xe2\x96\x82\xe2\x96\x84 ";
  if (dbm >= -80)
    return "\xe2\x96\x82 ";
  return "";
}

static const char *wlan_display_name(const struct clock_state *ci)
{
  const struct net_ctx *n = &ci->keep.net;
  if (n->wlan_idx < 0)
    return "";
  const char *name = name_idx_by_idx((struct rtnl_ctx *)&ci->keep.rtnl, (unsigned)n->ifindex[n->wlan_idx]);
  if (!name)
    name = "";
  return ci->keep.net.wlan_ssid[0] ? ci->keep.net.wlan_ssid : name;
}

static int fmt_net_ssid(char *dst, size_t sz, const struct clock_state *ci)
{
  const struct net_ctx *n = &ci->keep.net;
  char rate[RATE_LN_SZ] = "";
  if (n->wlan_rx_rate > 0 || n->wlan_tx_rate > 0)
    fmt_rate_str(rate, sizeof rate, n->wlan_rx_rate, n->wlan_tx_rate);
  return snprintf(dst, sz, "%s%s%s", ci->keep.text ? "SSID " : "\xf0\x9f\x9b\x9c ", wlan_display_name(ci), rate);
}

static const char *wlan_name(const struct clock_state *ci)
{
  const struct net_ctx *n = &ci->keep.net;
  if (n->wlan_idx < 0)
    return "";
  const char *name = name_idx_by_idx((struct rtnl_ctx *)&ci->keep.rtnl, (unsigned)n->ifindex[n->wlan_idx]);
  return name ? name : "";
}

static void fmt_line(const struct clock_state *ci, const char *r, const char *t, int pad, char **pp)
{
  char ssid_part[WLAN_LN_SZ];
  fmt_net_ssid(ssid_part, sizeof ssid_part, ci);
  const char *b = fmt_bars(ci->net_dbm);
  const char *name = wlan_name(ci);
  fmt_into(pp, net_line_rem(ci, pp), "%s \xe2\x86\x93%s\xe2\x86\x91%s%*s%s%ddBm\n%s",
           name, r, t, pad, "", b, ci->net_dbm, ssid_part);
}

static void fmt_wlan_line(const struct clock_state *ci, const char *r, const char *t, const int lens[2], char **pp)
{
  int pad = ci->keep.text ? 1 : wifi_pad(lens[0], lens[1]);
  fmt_line(ci, r, t, pad, pp);
}

static void fmt_wired_lines(const struct clock_state *ci, const unsigned long long *rd, const unsigned long long *td,
                            int w, char **pp)
{
  for (unsigned i = 0; i < ci->keep.net.count; i++) {
    if ((int)i == w)
      continue;
    fmt_iface_line(ci, (int)i, rd[i], td[i], pp);
    if (net_line_rem(ci, pp) > 1)
      *(*pp)++ = '\n';
  }
}

static void append_wlan_line(const struct clock_state *ci, char **p, const unsigned long long *rd,
                             const unsigned long long *td, int w)
{
  if (*p > ci->net_line && (*p)[-1] != '\n' && net_line_rem(ci, p) > 1)
    *(*p)++ = '\n';
  char r[RATE_SZ], t[RATE_SZ];
  int lr = net_fmt_thr(r, sizeof r, rd[w]);
  int lt = net_fmt_thr(t, sizeof t, td[w]);
  fmt_wlan_line(ci, r, t, (int[]) { lr, lt }, p);
}

static void fmt_net_line(struct clock_state *ci, const unsigned long long *rd, const unsigned long long *td)
{
  char *p = ci->net_line;
  *p = 0;
  int w = ci->keep.net.wlan_idx;
  fmt_wired_lines(ci, rd, td, w, &p);
  if (w >= 0)
    append_wlan_line(ci, &p, rd, td, w);
}

static void query_station_rate(struct clock_state *ci)
{
  if (ci->keep.net.wlan_idx < 0 || ci->keep.nlk.fd < 0)
    return;
  const struct net_ctx *n = &ci->keep.net;
  const char *w = name_idx_by_idx(&ci->keep.rtnl, (unsigned)n->ifindex[n->wlan_idx]);
  if (!w)
    return;
  nlk_station_rate(&ci->keep.nlk, w, &ci->keep.net.wlan_rx_rate, &ci->keep.net.wlan_tx_rate, &ci->keep.net.wlan_dbm);
}

void do_wlan(struct clock_state *ci)
{
  if (ci->keep.net.count == 0)
    return;
  find_wlan_nlk(ci);
  query_station_rate(ci);
}

static int fetch_dbm(struct clock_state *ci)
{
  int dbm = read_dbm(&ci->keep.net);
  if (dbm)
    ci->net_dbm = dbm;
  return ci->net_dbm;
}

void read_net_dev(struct clock_state *ci, struct rtnl_ctx *r)
{
  unsigned long long rd[MAX_NET] = { 0 }, td[MAX_NET] = { 0 };
  rtnl_read_dev(r, &ci->keep.net, rd, td);
  int fd = get_dgram_fd(&ci->keep.net);
  if (fd > 0)
    read_link_speed(r, &ci->keep.net, fd);
  fetch_dbm(ci);
  fmt_net_line(ci, rd, td);
}
