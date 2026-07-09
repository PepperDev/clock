#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <linux/netlink.h>      // cppcheck-suppress missingIncludeSystem
#include <linux/rtnetlink.h>    // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"

struct def_route_arg {
  struct rtnl_ctx *ctx;
  char *out;
  int sz;
  int best;
  int family;
  int *gw_idx;
  int *gw_metric;
  int gw_cap;
  int gw_n;
};

static int route_oif_metric(const struct nlmsghdr *nh, int *oif, unsigned *metric)
{
  struct rtmsg *r = (struct rtmsg *)NLMSG_DATA(nh);
  struct rtattr *rta;
  size_t rtaspace = RTM_PAYLOAD(nh);
  for (rta = RTM_RTA(r); RTA_OK(rta, rtaspace); rta = RTA_NEXT(rta, rtaspace)) {
    if (rta->rta_type == RTA_OIF)
      *oif = *(int *)RTA_DATA(rta);
    else if (rta->rta_type == RTA_PRIORITY)
      *metric = *(unsigned *)RTA_DATA(rta);
  }
  return *oif >= 0 ? 0 : -1;
}

static void record_gw(struct def_route_arg *a, int oif, unsigned metric)
{
  if (!a->gw_idx)
    return;
  for (int i = 0; i < a->gw_n; i++)
    if (a->gw_idx[i] == oif)
      return;
  if (a->gw_n < a->gw_cap) {
    a->gw_idx[a->gw_n] = oif;
    if (a->gw_metric)
      a->gw_metric[a->gw_n] = (int)metric;
    a->gw_n++;
  }
}

static void update_best(struct def_route_arg *a, int oif, unsigned metric)
{
  if (metric < (unsigned)a->best) {
    a->best = (int)metric;
    const char *name = name_idx_by_idx(a->ctx, (unsigned)oif);
    if (name)
      snprintf(a->out, a->sz, "%s", name);
  }
}

static int def_route_cb(const struct nlmsghdr *nh, void *arg)
{
  struct def_route_arg *a = (struct def_route_arg *)arg;
  const struct rtmsg *r = (const struct rtmsg *)NLMSG_DATA(nh);
  if (r->rtm_family != a->family || r->rtm_dst_len != 0)
    return 0;
  int oif = -1;
  unsigned metric = ~0U;
  route_oif_metric(nh, &oif, &metric);
  if (oif >= 0) {
    if (metric == ~0U)
      metric = 0;
    update_best(a, oif, metric);
    record_gw(a, oif, metric);
  }
  return 0;
}

int rtnl_default_v6(struct rtnl_ctx *r, char *out, int sz)
{
  if (r->v6_cached) {
    if (r->v6_cache[0]) {
      snprintf(out, sz, "%s", r->v6_cache);
      return 0;
    }
    return -1;
  }
  r->v6_cached = 1;
  struct def_route_arg a = {.ctx = r,.out = out,.sz = sz,.best = -1,.family = AF_INET6 };
  if (rtnl_route_dump(r, AF_INET6, def_route_cb, &a) < 0)
    return -1;
  if (a.best >= 0)
    snprintf(r->v6_cache, sizeof r->v6_cache, "%s", out);
  return a.best >= 0 ? 0 : -1;
}

static struct def_route_arg def_route_arg_init(struct rtnl_ctx *r, char *out, int sz, struct gw_track *gw, int family)
{
  struct def_route_arg a = { 0 };
  a.ctx = r;
  a.out = out;
  a.sz = sz;
  a.best = -1;
  a.family = family;
  a.gw_cap = gw ? gw->cap : 0;
  if (gw) {
    a.gw_n = gw->n;
    a.gw_idx = gw->idx;
    a.gw_metric = gw->metric;
  }
  return a;
}

int rtnl_default_v4_idx(struct rtnl_ctx *r, char *out, int sz, struct gw_track *gw)
{
  struct def_route_arg a = def_route_arg_init(r, out, sz, gw, AF_INET);
  if (rtnl_route_dump(r, AF_INET, def_route_cb, &a) < 0)
    return -1;
  if (gw)
    gw->n = a.gw_n;
  return a.best >= 0 ? 0 : -1;
}

int rtnl_default_v6_idx(struct rtnl_ctx *r, char *out, int sz, struct gw_track *gw)
{
  struct def_route_arg a = def_route_arg_init(r, out, sz, gw, AF_INET6);
  if (rtnl_route_dump(r, AF_INET6, def_route_cb, &a) < 0)
    return -1;
  r->v6_cached = 1;
  if (a.best < 0)
    return -1;
  snprintf(r->v6_cache, sizeof r->v6_cache, "%s", out);
  if (gw)
    gw->n = a.gw_n;
  return 0;
}
