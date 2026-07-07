#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <limits.h>             // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <linux/netlink.h>      // cppcheck-suppress missingIncludeSystem
#include <linux/rtnetlink.h>    // cppcheck-suppress missingIncludeSystem
#include <linux/if_link.h>      // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"

#define RTNL_BUF 16384

static void link_get_stats(const struct rtattr *rta, unsigned long long *rx, unsigned long long *tx)
{
  struct rtnl_link_stats64 s;
  if (RTA_PAYLOAD(rta) < sizeof s) {
    *rx = 0;
    *tx = 0;
    return;
  }
  memcpy(&s, RTA_DATA(rta), sizeof s);
  *rx = s.rx_bytes;
  *tx = s.tx_bytes;
}

static int is_virtual_kind(struct rtattr *rta)
{
  static const char *VTYPES[] = {
    "tun", "tap", "veth", "bridge", "bond", "dummy",
    "sit", "gre", "gretap", "vti", "vlan", "vxlan", "geneve"
  };
  for (size_t k = 0; k < sizeof VTYPES / sizeof VTYPES[0]; k++)
    if (strcmp((const char *)RTA_DATA(rta), VTYPES[k]) == 0)
      return 1;
  return 0;
}

static void parse_linkinfo(struct rtattr *rta, struct link_entry *e)
{
  size_t laspace = RTA_PAYLOAD(rta);
  for (struct rtattr * la = (struct rtattr *)RTA_DATA(rta); RTA_OK(la, laspace); la = RTA_NEXT(la, laspace)) {
    if (la->rta_type != IFLA_INFO_KIND)
      continue;
    if (is_virtual_kind(la))
      e->is_virtual = 1;
  }
}

static void read_link_attrs(struct link_entry *e, const struct nlmsghdr *nh)
{
  const struct ifinfomsg *ifi = (const struct ifinfomsg *)NLMSG_DATA(nh);
  struct rtattr *rta;
  size_t rtaspace = IFLA_PAYLOAD(nh);
  for (rta = IFLA_RTA(ifi); RTA_OK(rta, rtaspace); rta = RTA_NEXT(rta, rtaspace)) {
    if (rta->rta_type == IFLA_IFNAME)
      snprintf(e->name, sizeof(e->name), "%s", (const char *)RTA_DATA(rta));
    if (rta->rta_type == IFLA_STATS64)
      link_get_stats(rta, &e->rx, &e->tx);
    if (rta->rta_type == IFLA_LINKINFO)
      parse_linkinfo(rta, e);
  }
}

static int link_grow(struct rtnl_ctx *r)
{
  int cap = r->link_cap;
  int need = 1;
  int step = cap > 65536 ? 65536 : cap > 64 ? cap : 64;
  if (step < need)
    step = need;
  int new_cap = cap + step;
  if (new_cap > INT_MAX - 64)
    return -1;
  new_cap = (int)(((unsigned)new_cap + 63) & ~63U);
  struct link_entry *n = realloc(r->links, (unsigned)new_cap * sizeof *n);
  if (!n)
    return -1;
  r->links = n;
  r->link_cap = new_cap;
  return 0;
}

static int link_cache_cb(const struct nlmsghdr *nh, void *arg)
{
  struct rtnl_ctx *r = (struct rtnl_ctx *)arg;
  const struct ifinfomsg *ifi = (const struct ifinfomsg *)NLMSG_DATA(nh);
  if (r->link_count >= r->link_cap && link_grow(r) < 0)
    return 0;
  struct link_entry *e = &r->links[r->link_count];
  memset(e, 0, sizeof(*e));
  e->ifindex = ifi->ifi_index;
  e->speed = -1;
  if (e->ifindex == 1)
    e->is_virtual = 1;
  read_link_attrs(e, nh);
  if (e->name[0])
    r->link_count++;
  return 0;
}

void rtnl_cache_links(struct rtnl_ctx *r)
{
  r->link_count = 0;
  rtnl_dump(r, RTM_GETLINK, AF_PACKET, link_cache_cb, r);
  r->links_stale = 0;
  r->refreshed = 1;
}

static int find_cache_idx(const struct rtnl_ctx *r, const char *name)
{
  for (int i = 0; i < r->link_count; i++)
    if (strcmp(r->links[i].name, name) == 0)
      return i;
  return -1;
}

static int find_cache_idx_by_ifindex(const struct rtnl_ctx *r, int ifindex)
{
  for (int i = 0; i < r->link_count; i++)
    if (r->links[i].ifindex == ifindex)
      return i;
  return -1;
}

static int parse_link_rta(struct nlmsghdr *rnh, char *name, size_t nsz, unsigned long long *rx, unsigned long long *tx)
{
  const struct ifinfomsg *rfi = (const struct ifinfomsg *)NLMSG_DATA(rnh);
  struct rtattr *rta;
  size_t rtaspace = IFLA_PAYLOAD(rnh);
  for (rta = IFLA_RTA(rfi); RTA_OK(rta, rtaspace); rta = RTA_NEXT(rta, rtaspace)) {
    if (rta->rta_type == IFLA_IFNAME)
      snprintf(name, nsz, "%s", (const char *)RTA_DATA(rta));
    else if (rta->rta_type == IFLA_STATS64)
      link_get_stats(rta, rx, tx);
  }
  return name[0] ? 0 : -1;
}

static int process_link_resp(struct rtnl_ctx *r, struct nlmsghdr *rnh,
                             unsigned long long *rx, unsigned long long *tx, char *name)
{
  if (rnh->nlmsg_type == NLMSG_DONE)
    return -1;
  if (parse_link_rta(rnh, name, IFACE_NAME_LEN, rx, tx) < 0)
    return -1;
  int idx = find_cache_idx(r, name);
  if (idx >= 0) {
    r->links[idx].rx = *rx;
    r->links[idx].tx = *tx;
  }
  return 0;
}

static int do_link_query(struct rtnl_ctx *r, unsigned ifindex, unsigned char *resp)
{
  struct nlmsghdr *nh = (struct nlmsghdr *)resp;
  memset(resp, 0, RTNL_BUF);
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
  nh->nlmsg_type = RTM_GETLINK;
  nh->nlmsg_flags = NLM_F_REQUEST;
  nh->nlmsg_seq = ++r->seq;
  struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nh);
  ifi->ifi_family = AF_PACKET;
  ifi->ifi_index = (int)ifindex;
  if (nl_send_dump(r->fd, resp, nh->nlmsg_len) < 0)
    return -1;
  ssize_t n;
  return nlk_recv_one(r, resp, RTNL_BUF, &n);
}

static void update_one(struct rtnl_ctx *r, unsigned ifindex)
{
  unsigned char buf[RTNL_BUF];
  if (do_link_query(r, ifindex, buf) < 0)
    return;
  unsigned long long rx = 0, tx = 0;
  char name[IFACE_NAME_LEN] = "";
  if (process_link_resp(r, (struct nlmsghdr *)buf, &rx, &tx, name) < 0) {
    nl_drain_done(r->fd);
    return;
  }
  nl_drain_done(r->fd);
}

static void update_deltas(struct rtnl_ctx *r, struct net_ctx *c, unsigned long long *rd, unsigned long long *td, int i)
{
  unsigned long long rx = r->links[c->link_idx[i]].rx, tx = r->links[c->link_idx[i]].tx;
  if (c->rp[i] && rx >= c->rp[i])
    rd[i] = rx - c->rp[i];
  if (c->tp[i] && tx >= c->tp[i])
    td[i] = tx - c->tp[i];
  c->rp[i] = rx, c->tp[i] = tx;
}

static void scan_idx(struct rtnl_ctx *r, struct net_ctx *c, int full)
{
  if (full)
    rtnl_cache_links(r);
  int skip = r->refreshed;
  r->refreshed = 0;
  for (int i = 0; i < (int)c->count; i++)
    c->link_idx[i] = find_cache_idx_by_ifindex(r, c->ifindex[i]);
  if (skip)
    return;
  for (int i = 0; i < (int)c->count; i++)
    if (c->link_idx[i] >= 0)
      update_one(r, (unsigned)r->links[c->link_idx[i]].ifindex);
}

void rtnl_read_dev(struct rtnl_ctx *r, struct net_ctx *c, unsigned long long *rd, unsigned long long *td)
{
  scan_idx(r, c, r->links_stale | (r->link_count == 0));
  for (int i = 0; i < (int)c->count; i++)
    if (c->link_idx[i] >= 0)
      update_deltas(r, c, rd, td, i);
}
