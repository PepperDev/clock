#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <linux/netlink.h>      // cppcheck-suppress missingIncludeSystem
#include <linux/rtnetlink.h>    // cppcheck-suppress missingIncludeSystem
#include <linux/if_link.h>      // cppcheck-suppress missingIncludeSystem
#include <linux/if_addr.h>      // cppcheck-suppress missingIncludeSystem
#include <errno.h>              // cppcheck-suppress missingIncludeSystem
#include <limits.h>             // cppcheck-suppress missingIncludeSystem
#include <arpa/inet.h>          // cppcheck-suppress missingIncludeSystem
#include <netinet/ip6.h>        // cppcheck-suppress missingIncludeSystem
#include <poll.h>               // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "main.h"
#include "monitor_int.h"
#include "monitor.h"

#define RTNL_BUF 16384
#define IPV6_GLOBAL_MASK 0xe0
#define IPV6_GLOBAL_PREFIX 0x20

int rtnl_open(struct rtnl_ctx *r)
{
  r->fd = sys_socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (r->fd < 0)
    return -1;
  int one = 1;
  setsockopt(r->fd, SOL_NETLINK, NETLINK_GET_STRICT_CHK, &one, sizeof one);
  struct sockaddr_nl sa = {.nl_family = AF_NETLINK };
  if (sys_bind(r->fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
    sys_close(r->fd);
    return -1;
  }
  r->seq = 0;
  return 0;
}

int nlk_recv_one(const struct rtnl_ctx *r, unsigned char *buf, size_t sz, ssize_t *np)
{
  struct pollfd pfd = {.fd = r->fd,.events = POLLIN };
  int pr;
  do {
    pr = sys_poll(&pfd, 1, POLL_WAIT_MS);
    if (tls_terminated)
      return -1;
  } while (pr < 0 && errno == EINTR);
  if (pr <= 0)
    return -1;
  struct iovec ri = {.iov_base = buf,.iov_len = sz };
  struct msghdr rm = {.msg_iov = &ri,.msg_iovlen = 1 };
  *np = nlk_recv(r->fd, &rm, MSG_DONTWAIT);
  return *np < 0 ? -1 : 0;
}

static int nlk_recv_loop(const struct rtnl_ctx *r, rtnl_cb cb, void *arg)
{
  int budget = 64;
  unsigned char buf[RTNL_BUF];
  for (;;) {
    if (--budget == 0)
      return -1;
    ssize_t n;
    if (nlk_recv_one(r, buf, sizeof buf, &n) < 0)
      return -1;
    int st = nl_handle_buf(buf, (size_t)n, cb, arg);
    if (st <= 0)
      return st;
    if (st != 2)
      continue;
    nl_drain_done(r->fd);
    return 0;
  }
}

static void rtnl_set_dump_hdr(struct nlmsghdr *nh, int type, int family)
{
  nh->nlmsg_type = (unsigned short)type;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  nh->nlmsg_pid = 0;
  if (type == RTM_GETADDR) {
    struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nh);
    ifa->ifa_family = (unsigned char)family;
    nh->nlmsg_len = sizeof(struct nlmsghdr) + sizeof(struct ifaddrmsg);
  } else {
    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nh);
    ifi->ifi_family = (unsigned char)family;
    nh->nlmsg_len = sizeof(struct nlmsghdr) + sizeof(struct ifinfomsg);
  }
}

int rtnl_dump(struct rtnl_ctx *r, int type, int family, rtnl_cb cb, void *arg)
{
  unsigned char req[CUP_BUF_SZ] = { 0 };
  struct nlmsghdr *nh = (struct nlmsghdr *)req;
  nh->nlmsg_seq = ++r->seq;
  rtnl_set_dump_hdr(nh, type, family);
  if (nl_send_dump(r->fd, req, nh->nlmsg_len) < 0)
    return -1;
  return nlk_recv_loop(r, cb, arg);
}

int rtnl_route_dump(struct rtnl_ctx *r, int family, rtnl_cb cb, void *arg)
{
  unsigned char req[RTNL_REQ_SZ] = { 0 };
  struct nlmsghdr *nh = (struct nlmsghdr *)req;
  struct rtmsg *rt = (struct rtmsg *)NLMSG_DATA(nh);
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
  nh->nlmsg_type = RTM_GETROUTE;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  nh->nlmsg_seq = ++r->seq;
  nh->nlmsg_pid = 0;
  rt->rtm_family = (unsigned char)family;
  rt->rtm_table = RT_TABLE_MAIN;
  if (nl_send_dump(r->fd, req, nh->nlmsg_len) < 0)
    return -1;
  return nlk_recv_loop(r, cb, arg);
}

/* ── ifindex↔ifname resolution cache ── */

static int name_idx_grow(struct rtnl_ctx *r)
{
  int cap = r->name_idx_cap;
  int need = 1;
  int step = cap > 65536 ? 65536 : cap > 64 ? cap : 64;
  if (step < need)
    step = need;
  int new_cap = cap + step;
  if (new_cap > INT_MAX - 64)
    return -1;
  new_cap = (int)(((unsigned)new_cap + 63) & ~63U);
  struct name_idx *p = realloc(r->name_idx, (size_t)new_cap * sizeof(struct name_idx));
  if (!p)
    return -1;
  r->name_idx = p;
  r->name_idx_cap = new_cap;
  return 0;
}

const char *name_idx_by_idx(struct rtnl_ctx *r, unsigned ifindex)
{
  for (int i = 0; i < r->name_idx_n; i++)
    if (r->name_idx[i].ifindex == ifindex)
      return r->name_idx[i].name;
  char name[IFACE_NAME_LEN];
  if (!sys_if_indextoname(ifindex, name))
    return NULL;
  int i = r->name_idx_n;
  if (i >= r->name_idx_cap && name_idx_grow(r) < 0)
    return NULL;
  r->name_idx[i].ifindex = ifindex;
  memcpy(r->name_idx[i].name, name, IFACE_NAME_LEN);
  r->name_idx_n = i + 1;
  return r->name_idx[i].name;
}

unsigned name_idx_by_name(struct rtnl_ctx *r, const char *name)
{
  for (int i = 0; i < r->name_idx_n; i++)
    if (strcmp(r->name_idx[i].name, name) == 0)
      return r->name_idx[i].ifindex;
  unsigned idx = sys_if_nametoindex(name);
  if (!idx)
    return 0;
  int i = r->name_idx_n;
  if (i >= r->name_idx_cap && name_idx_grow(r) < 0)
    return idx;
  r->name_idx[i].ifindex = idx;
  memcpy(r->name_idx[i].name, name, IFACE_NAME_LEN);
  r->name_idx_n = i + 1;
  return idx;
}

/* ── IPv6 address ── */
struct addr6_arg {
  unsigned ifindex;
  char ip6[INET6_ADDRSTRLEN];
  int found;
};

static int addr6_attrs(const struct nlmsghdr *nh, struct in6_addr *addr)
{
  const struct ifaddrmsg *ifa = (const struct ifaddrmsg *)NLMSG_DATA(nh);
  struct rtattr *rta;
  size_t rtaspace = IFA_PAYLOAD(nh);
  int have = 0;
  for (rta = IFA_RTA(ifa); RTA_OK(rta, rtaspace); rta = RTA_NEXT(rta, rtaspace)) {
    if (rta->rta_type == IFA_ADDRESS && RTA_PAYLOAD(rta) >= sizeof(struct in6_addr)) {
      memcpy(addr, RTA_DATA(rta), sizeof(*addr));
      have = 1;
    }
  }
  return have ? 0 : -1;
}

static int addr6_cb(const struct nlmsghdr *nh, void *arg)
{
  struct addr6_arg *a = (struct addr6_arg *)arg;
  const struct ifaddrmsg *ifa = (const struct ifaddrmsg *)NLMSG_DATA(nh);
  if (ifa->ifa_family != AF_INET6 || ifa->ifa_index != a->ifindex)
    return 0;
  struct in6_addr addr;
  if (addr6_attrs(nh, &addr) < 0)
    return 0;
  if ((addr.s6_addr[0] & IPV6_GLOBAL_MASK) != IPV6_GLOBAL_PREFIX)
    return 0;
  const char *s = inet_ntop(AF_INET6, &addr, a->ip6, sizeof a->ip6);
  if (s) {
    a->found = 1;
    return 1;
  }
  return 0;
}

int rtnl_find_addr6(struct rtnl_ctx *r, const char *target, char *ip6, size_t sz)
{
  unsigned idx = name_idx_by_name(r, target);
  if (!idx)
    return -1;
  struct addr6_arg a = {.ifindex = idx,.found = 0 };
  if (rtnl_dump(r, RTM_GETADDR, AF_INET6, addr6_cb, &a) < 0)
    return -1;
  if (!a.found)
    return -1;
  snprintf(ip6, sz, "%s", a.ip6);
  return 0;
}
