#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <linux/netlink.h>      // cppcheck-suppress missingIncludeSystem
#include <linux/genetlink.h>    // cppcheck-suppress missingIncludeSystem
#include <linux/nl80211.h>      // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"

#define NLA_I8(a) (*(const signed char *)((const char *)(a) + NLA_HDRLEN))

static void pstr(unsigned char *p, unsigned short t, const char *s, size_t sl)
{
  struct nlattr *a = (struct nlattr *)p;
  a->nla_type = t;
  a->nla_len = NLA_HDRLEN + (unsigned short)sl;
  memcpy((char *)a + NLA_HDRLEN, s, sl);
}

static void pu32(unsigned char *p, unsigned short t, const void *v)
{
  struct nlattr *a = (struct nlattr *)p;
  a->nla_type = t;
  a->nla_len = NLA_HDRLEN + (unsigned short)sizeof(int);
  memcpy((char *)a + NLA_HDRLEN, v, sizeof(int));
}

static int u16v(const struct nlattr *a)
{
  return *(unsigned short *)((char *)a + NLA_HDRLEN);
}

static int u32v(const struct nlattr *a)
{
  return (int)*(unsigned int *)((char *)a + NLA_HDRLEN);
}

static const struct nlattr *walk(const struct nlmsghdr *nh, unsigned short t)
{
  if (!NLMSG_OK(nh, nh->nlmsg_len) || nh->nlmsg_type == NLMSG_ERROR)
    return NULL;
  size_t p = NLMSG_SPACE(GENL_HDRLEN);
  while (p < nh->nlmsg_len) {
    const struct nlattr *a = (const struct nlattr *)((const char *)nh + p);
    if ((a->nla_type & NLA_TYPE_MASK) == t)
      return a;
    p += NLA_ALIGN(a->nla_len);
  }
  return NULL;
}

static const struct nlattr *nested(const struct nlattr *parent, unsigned short t)
{
  size_t p = NLA_HDRLEN;
  while (p < parent->nla_len) {
    const struct nlattr *a = (const struct nlattr *)((const char *)parent + p);
    if ((a->nla_type & NLA_TYPE_MASK) == t)
      return a;
    p += NLA_ALIGN(a->nla_len);
  }
  return NULL;
}

static int bitrate_val(const struct nlattr *parent)
{
  const struct nlattr *a = nested(parent, NL80211_RATE_INFO_BITRATE32);
  if (a)
    return u32v(a);
  a = nested(parent, NL80211_RATE_INFO_BITRATE);
  return a ? u16v(a) : -1;
}

static int do_resolve(struct netlink_ctx *nlk)
{
  unsigned char b[NLBUF] = { 0 };
  struct nlmsghdr *nh = (struct nlmsghdr *)b;
  struct genlmsghdr *gh = NLMSG_DATA(nh);
  nh->nlmsg_len = sizeof(struct nlmsghdr) + GENL_HDRLEN + NLA_HDRLEN + sizeof("nl80211");
  init_req(nh, gh, nlk, GENL_ID_CTRL, CTRL_CMD_GETFAMILY);
  pstr((unsigned char *)gh + GENL_HDRLEN, CTRL_ATTR_FAMILY_NAME, "nl80211", sizeof("nl80211"));
  if (talk(nlk, b) < 0)
    return -1;
  const struct nlattr *a = walk(nh, CTRL_ATTR_FAMILY_ID);
  return a ? u16v(a) : -1;
}

static int ensure_resolved(struct netlink_ctx *nlk)
{
  if (nlk->family >= 0)
    return 0;
  nlk->family = do_resolve(nlk);
  return nlk->family >= 0 ? 0 : -1;
}

static int iface_to_idx(struct netlink_ctx *nlk, const char *iface, int *idx)
{
  if (nlk->ifindex > 0) {
    *idx = nlk->ifindex;
    return 0;
  }
  unsigned i = sys_if_nametoindex(iface);
  if (!i)
    return -1;
  *idx = (int)i;
  nlk->ifindex = *idx;
  return 0;
}

static int copy_nla_str(const struct nlattr *a, char *buf, size_t sz)
{
  if (!a)
    return -1;
  size_t sl = a->nla_len - NLA_HDRLEN;
  if (sl >= sz)
    sl = sz - 1;
  memcpy(buf, (const char *)a + NLA_HDRLEN, sl);
  buf[sl] = 0;
  return 0;
}

static const struct nlmsghdr *do_nl80211(struct netlink_ctx *nlk, unsigned char *buf, int ifindex, int cmd)
{
  struct nlmsghdr *nh = (struct nlmsghdr *)buf;
  nh->nlmsg_len = sizeof(struct nlmsghdr) + GENL_HDRLEN + NLA_HDRLEN + sizeof(int);
  init_req(nh, NLMSG_DATA(nh), nlk, nlk->family, cmd);
  pu32((unsigned char *)NLMSG_DATA(nh) + GENL_HDRLEN, NL80211_ATTR_IFINDEX, &ifindex);
  if (talk(nlk, buf) < 0)
    return NULL;
  return nh;
}

static void set_rate(const struct nlattr *si, int type, int *out, int *ok)
{
  const struct nlattr *a = nested(si, type);
  if (!a)
    return;
  int v = bitrate_val(a);
  if (v >= 0) {
    *out = v * PERCENT_BASE / KHZ_PER_MHZ;
    *ok = 1;
  }
}

static int send_station_req(struct netlink_ctx *nlk, void *b, int idx)
{
  struct nlmsghdr *nh = (struct nlmsghdr *)b;
  nh->nlmsg_len = sizeof(struct nlmsghdr) + GENL_HDRLEN + NLA_HDRLEN + sizeof(int);
  init_req(nh, NLMSG_DATA(nh), nlk, nlk->family, NL80211_CMD_GET_STATION);
  nh->nlmsg_flags |= NLM_F_DUMP;
  pu32((unsigned char *)NLMSG_DATA(nh) + GENL_HDRLEN, NL80211_ATTR_IFINDEX, &idx);
  return talk_dump(nlk, b);
}

static int query_station(struct netlink_ctx *nlk, int idx, int *rx, int *tx, int *dbm)
{
  unsigned char b[NLBUF] = { 0 };
  if (send_station_req(nlk, b, idx) < 0)
    return -1;
  const struct nlattr *si = walk((struct nlmsghdr *)b, NL80211_ATTR_STA_INFO);
  if (!si)
    return -1;
  int ok = 0;
  set_rate(si, NL80211_STA_INFO_TX_BITRATE, tx, &ok);
  set_rate(si, NL80211_STA_INFO_RX_BITRATE, rx, &ok);
  if (dbm) {
    const struct nlattr *s = nested(si, NL80211_STA_INFO_SIGNAL);
    if (s)
      *dbm = NLA_I8(s);
  }
  return ok ? 0 : -1;
}

int nlk_wlan_ssid(struct netlink_ctx *nlk, const char *iface, char *ssid, size_t sz)
{
  int idx;
  if (ensure_resolved(nlk) < 0 || iface_to_idx(nlk, iface, &idx) < 0)
    return -1;
  unsigned char b[NLBUF] = { 0 };
  const struct nlmsghdr *nh = do_nl80211(nlk, b, idx, NL80211_CMD_GET_INTERFACE);
  if (!nh)
    return -1;
  return copy_nla_str(walk(nh, NL80211_ATTR_SSID), ssid, sz);
}

int nlk_station_rate(struct netlink_ctx *nlk, const char *iface, int *rx_mbps, int *tx_mbps, int *dbm)
{
  int idx;
  if (ensure_resolved(nlk) < 0 || iface_to_idx(nlk, iface, &idx) < 0)
    return -1;
  unsigned char b[NLBUF] = { 0 };
  const struct nlmsghdr *nh = do_nl80211(nlk, b, idx, NL80211_CMD_GET_STATION);
  if (!nh)
    return -1;
  return query_station(nlk, idx, rx_mbps, tx_mbps, dbm);
}

/* ── WLAN station-mode interface dump ── */

static int wlan_next_want(int cap, int need)
{
  int step = cap > 65536 ? 65536 : cap > 64 ? cap : 64;
  int want = cap + step;
  return want < need ? need : want;
}

static int wlan_cache_grow(struct wlan_cache *cache, int need)
{
  int cap = cache->cap;
  if (need <= cap)
    return 0;
  unsigned nc = (unsigned)((wlan_next_want(cap, need) + 63) & ~63);
  int *ni = realloc(cache->ifindices, nc * sizeof *ni);
  if (!ni)
    return -1;
  void *ns = realloc(cache->ssids, nc * SSID_SZ);
  if (!ns) {
    free(ni);
    return -1;
  }
  cache->ifindices = ni;
  cache->ssids = ns;
  cache->cap = (int)nc;
  return 0;
}

static int wlan_find_all_cb(const struct nlmsghdr *nh, void *arg)
{
  struct wlan_cache *cache = (struct wlan_cache *)arg;
  const struct nlattr *t = walk(nh, NL80211_ATTR_IFTYPE);
  if (!t || u32v(t) != NL80211_IFTYPE_STATION)
    return 0;
  const struct nlattr *idx_a = walk(nh, NL80211_ATTR_IFINDEX);
  if (!idx_a)
    return 0;
  if (cache->count >= cache->cap && wlan_cache_grow(cache, cache->count + 1) < 0)
    return 0;
  cache->ifindices[cache->count] = u32v(idx_a);
  copy_nla_str(walk(nh, NL80211_ATTR_SSID), cache->ssids[cache->count], SSID_SZ);
  cache->count++;
  return 0;
}

int nlk_find_wlan_all(struct netlink_ctx *nlk, struct wlan_cache *cache)
{
  if (ensure_resolved(nlk) < 0)
    return -1;
  cache->count = 0;
  cache->fresh = 1;
  return nl_dump_iter(nlk, NL80211_CMD_GET_INTERFACE, wlan_find_all_cb, cache);
}
