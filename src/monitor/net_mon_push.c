#define _GNU_SOURCE
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <linux/netlink.h>      // cppcheck-suppress missingIncludeSystem
#include <linux/rtnetlink.h>    // cppcheck-suppress missingIncludeSystem
#include <linux/if_link.h>      // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"

static size_t fifo_next_cap(size_t cap)
{
  size_t step = cap > 65536 ? 65536 : cap > 64 ? cap : 64;
  return (cap + step + 63) & ~(size_t)63;
}

static int fifo_push(struct mon_action_fifo *f, const struct mon_action *act)
{
  if (pthread_mutex_lock(&f->lock) != 0)
    return -1;
  if (f->count >= f->cap) {
    size_t ncap = fifo_next_cap(f->cap);
    struct mon_action *n = realloc(f->items, ncap * sizeof(struct mon_action));
    if (!n && ncap > 0) {
      pthread_mutex_unlock(&f->lock);
      return -1;
    }
    f->items = n;
    f->cap = ncap;
  }
  f->items[f->count++] = *act;
  pthread_mutex_unlock(&f->lock);
  return 0;
}

static void extract_ifname(struct rtattr *rta, char *name, size_t sz)
{
  size_t nlen = RTA_PAYLOAD(rta);
  if (nlen >= sz)
    nlen = sz - 1;
  memcpy(name, RTA_DATA(rta), nlen);
  name[nlen] = 0;
}

static void extract_kind(struct rtattr *rta, char *kind, size_t sz)
{
  size_t laspace = RTA_PAYLOAD(rta);
  for (struct rtattr * la = (struct rtattr *)RTA_DATA(rta); RTA_OK(la, laspace); la = RTA_NEXT(la, laspace)) {
    if (la->rta_type != IFLA_INFO_KIND)
      continue;
    size_t klen = RTA_PAYLOAD(la);
    if (klen >= sz)
      klen = sz - 1;
    memcpy(kind, RTA_DATA(la), klen);
    kind[klen] = 0;
    return;
  }
}

static void extract_link_attrs(struct rtattr *rta, size_t rtaspace, struct mon_action_link *lk)
{
  for (; RTA_OK(rta, rtaspace); rta = RTA_NEXT(rta, rtaspace)) {
    if (rta->rta_type == IFLA_IFNAME)
      extract_ifname(rta, lk->name, sizeof lk->name);
    else if (rta->rta_type == IFLA_LINKINFO)
      extract_kind(rta, lk->kind, sizeof lk->kind);
  }
}

void push_link_action(struct rtnl_mon_ctx *m, const struct nlmsghdr *nh, enum mon_action_type type)
{
  const struct ifinfomsg *ifi = (const struct ifinfomsg *)NLMSG_DATA(nh);
  struct mon_action act;
  memset(&act, 0, sizeof act);
  act.type = type;
  act.arg.link.ifindex = ifi->ifi_index;
  if (type == MON_LINK_ADD)
    act.arg.link.flags = ifi->ifi_flags;
  extract_link_attrs(IFLA_RTA(ifi), IFLA_PAYLOAD(nh), &act.arg.link);
  fifo_push(&m->fifo, &act);
}

static void copy_addr_val(struct mon_action *act, const struct rtattr *rta, int family)
{
  size_t plen = RTA_PAYLOAD(rta);
  if (family == AF_INET) {
    if (plen >= sizeof(struct in_addr))
      memcpy(&act->arg.addr.addr.v4, RTA_DATA(rta), sizeof(struct in_addr));
  } else if (family == AF_INET6) {
    if (plen >= sizeof(struct in6_addr))
      memcpy(&act->arg.addr.addr.v6, RTA_DATA(rta), sizeof(struct in6_addr));
  }
}

static void extract_ifa_addr(struct mon_action *act, const struct nlmsghdr *nh, const struct ifaddrmsg *ifa)
{
  struct rtattr *rta;
  size_t rtaspace = IFA_PAYLOAD(nh);
  for (rta = IFA_RTA(ifa); RTA_OK(rta, rtaspace); rta = RTA_NEXT(rta, rtaspace)) {
    if (rta->rta_type == IFA_ADDRESS) {
      copy_addr_val(act, rta, ifa->ifa_family);
      break;
    }
  }
}

void push_addr_action(struct rtnl_mon_ctx *m, const struct nlmsghdr *nh, enum mon_action_type type)
{
  const struct ifaddrmsg *ifa = (const struct ifaddrmsg *)NLMSG_DATA(nh);
  struct mon_action act;
  memset(&act, 0, sizeof act);
  act.type = type;
  act.arg.addr.ifindex = ifa->ifa_index;
  act.arg.addr.prefixlen = ifa->ifa_prefixlen;
  act.arg.addr.family = ifa->ifa_family;
  extract_ifa_addr(&act, nh, ifa);
  fifo_push(&m->fifo, &act);
}

static int extract_oif(const struct rtattr *rta, size_t rtaspace)
{
  for (; RTA_OK(rta, rtaspace); rta = RTA_NEXT(rta, rtaspace))
    if (rta->rta_type == RTA_OIF && RTA_PAYLOAD(rta) >= sizeof(int))
      return *(int *)RTA_DATA(rta);
  return 0;
}

void push_route_action(struct rtnl_mon_ctx *m, const struct nlmsghdr *nh, enum mon_action_type type)
{
  const struct rtmsg *rtm = (const struct rtmsg *)NLMSG_DATA(nh);
  struct mon_action act;
  memset(&act, 0, sizeof act);
  act.type = type;
  act.arg.route.table = rtm->rtm_table;
  act.arg.route.dst_len = rtm->rtm_dst_len;
  act.arg.route.family = rtm->rtm_family;
  act.arg.route.ifindex = extract_oif(RTM_RTA(rtm), RTM_PAYLOAD(nh));
  fifo_push(&m->fifo, &act);
}
