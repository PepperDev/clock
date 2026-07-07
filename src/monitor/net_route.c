#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <arpa/inet.h>          // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <errno.h>              // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

static int is_distinct_v6(const char *v6, const char *v4)
{
  return v6[0] && strcmp(v6, v4) != 0;
}

static int gather_routes(struct rtnl_ctx *r, struct gw_track *gt, char *v4, char *v6)
{
  int sz = IFACE_NAME_LEN;
  int v4_ok = 0;
  if (rtnl_default_v4_idx(r, v4, sz, gt) >= 0)
    v4_ok = 1;
  if (!v4_ok)
    gt->n = 0;
  int n = gt->n;
  rtnl_default_v6_idx(r, v6, sz, gt);
  if (!v6[0])
    gt->n = n;
  if (!v4_ok) {
    if (!v6[0])
      return -1;
    memcpy(v4, v6, sz);
  }
  return 0;
}

static int classify_gw(const struct rtnl_ctx *r, const struct gw_track *gt, int *is_virt)
{
  int has_phys = 0;
  for (int i = 0; i < gt->n; i++) {
    is_virt[i] = 0;
    for (int j = 0; j < r->link_count; j++) {
      if (r->links[j].ifindex != gt->idx[i])
        continue;
      if (r->links[j].is_virtual)
        is_virt[i] = 1;
      else
        has_phys = 1;
      break;
    }
  }
  return has_phys;
}

static void compact_virtuals(struct gw_track *gt, const struct rtnl_ctx *r)
{
  int is_virt[MAX_NET];
  if (!classify_gw(r, gt, is_virt))
    return;
  int wp = 0;
  for (int i = 0; i < gt->n; i++) {
    if (is_virt[i])
      continue;
    gt->idx[wp] = gt->idx[i];
    gt->metric[wp] = gt->metric[i];
    wp++;
  }
  gt->n = wp;
}

static void pickup_gw_name(struct rtnl_ctx *r, char *name, const struct gw_track *gt)
{
  if (!name[0])
    return;
  unsigned idx = name_idx_by_name(r, name);
  if (!idx) {
    name[0] = 0;
    return;
  }
  for (int i = 0; i < gt->n; i++)
    if (gt->idx[i] == (int)idx)
      return;
  name[0] = 0;
}

static int fallback_v4(char *v4, const struct gw_track *gt, struct rtnl_ctx *r)
{
  for (int i = 0; i < gt->n; i++) {
    const char *n = name_idx_by_idx(r, (unsigned)gt->idx[i]);
    if (n) {
      memcpy(v4, n, IFACE_NAME_LEN);
      return 0;
    }
  }
  return -1;
}

static int resolve_gateway(struct net_ctx *c, struct rtnl_ctx *r, struct gw_track *gt, char *v4, char *v6)
{
  if (gather_routes(r, gt, v4, v6) < 0)
    return -1;
  compact_virtuals(gt, r);
  c->gw_n = gt->n;
  if (gt->n == 0)
    return -1;
  pickup_gw_name(r, v4, gt);
  pickup_gw_name(r, v6, gt);
  if (!v4[0]) {
    if (v6[0])
      memcpy(v4, v6, IFACE_NAME_LEN);
    else if (fallback_v4(v4, gt, r) < 0)
      return -1;
  }
  return 0;
}

static void scan_has_phys(struct net_ctx *c, const struct rtnl_ctx *r)
{
  c->has_phys = 0;
  for (unsigned i = 0; i < c->count; i++)
    for (int j = 0; j < r->link_count; j++)
      if (r->links[j].ifindex == c->ifindex[i] && !r->links[j].is_virtual) {
        c->has_phys = 1;
        return;
      }
}

static void set_primary_nics(struct net_ctx *c, struct rtnl_ctx *r, const struct gw_track *gt, const char *v4,
                             const char *v6)
{
  unsigned idx0 = name_idx_by_name(r, v4);
  c->ifindex[0] = idx0 ? (int)idx0 : (gt->n > 0 ? gt->idx[0] : 0);
  c->count = 1;
  if (is_distinct_v6(v6, v4)) {
    unsigned idx1 = name_idx_by_name(r, v6);
    if (idx1 && idx1 != (unsigned)c->ifindex[0]) {
      c->ifindex[1] = (int)idx1;
      c->count = 2;
    }
  }
  scan_has_phys(c, r);
}

static void pick_cached(struct net_ctx *c, struct rtnl_ctx *r, struct gw_track *gt, char *v4, char *v6)
{
  gt->n = c->gw_n;
  compact_virtuals(gt, r);
  c->gw_n = gt->n;
  if (gt->n == 0)
    return;
  const char *n0 = name_idx_by_idx(r, (unsigned)gt->idx[0]);
  if (n0)
    memcpy(v4, n0, IFACE_NAME_LEN);
  if (gt->n > 1) {
    const char *n1 = name_idx_by_idx(r, (unsigned)gt->idx[1]);
    if (n1 && strcmp(n1, v4) != 0)
      memcpy(v6, n1, IFACE_NAME_LEN);
  }
}

void pick_primary(struct net_ctx *c, struct rtnl_ctx *r, int use_cached)
{
  struct gw_track gt = {.idx = c->gw_idx,.metric = c->gw_metric,.cap = MAX_NET,.n = 0 };
  c->count = 0;
  c->wlan_idx = -1;
  c->has_phys = 0;
  r->v6_cache[0] = 0;
  r->v6_cached = 0;
  char v4[IFACE_NAME_LEN] = "", v6[IFACE_NAME_LEN] = "";
  if (use_cached)
    pick_cached(c, r, &gt, v4, v6);
  else if (resolve_gateway(c, r, &gt, v4, v6) < 0)
    return;
  set_primary_nics(c, r, &gt, v4, v6);
}

void get_local_ip6(struct clock_state *ci)
{
  if (ci->keep.net.count == 0 || ci->keep.rtnl.fd <= 0)
    return;
  const char *primary = name_idx_by_idx(&ci->keep.rtnl, (unsigned)ci->keep.net.ifindex[0]);
  if (!primary)
    return;
  char target[IFACE_NAME_LEN];
  memcpy(target, primary, IFACE_NAME_LEN);
  char v6_route[IFACE_NAME_LEN] = "";
  rtnl_default_v6(&ci->keep.rtnl, v6_route, sizeof v6_route);
  if (v6_route[0])
    memcpy(target, v6_route, IFACE_NAME_LEN);
  rtnl_find_addr6(&ci->keep.rtnl, target, ci->local_ip6, sizeof ci->local_ip6);
}

static int wlan_open_nlk(struct clock_state *ci)
{
  if (ci->keep.nlk.fd > 0)
    return 0;
  return nlk_init(&ci->keep.nlk);
}

static int wlan_better(const struct net_ctx *net, unsigned ifidx, unsigned *best)
{
  for (int j = 0; j < net->gw_n; j++)
    if (net->gw_idx[j] == (int)ifidx && (unsigned)net->gw_metric[j] < *best) {
      *best = (unsigned)net->gw_metric[j];
      return 1;
    }
  return 0;
}

static int ensure_wlan_slot_by_idx(struct net_ctx *net, int ifidx)
{
  for (unsigned k = 0; k < net->count; k++)
    if (net->ifindex[k] == ifidx)
      return k;
  if (net->count >= MAX_NET)
    return -1;
  int slot = (int)net->count;
  net->ifindex[slot] = ifidx;
  net->count++;
  return slot;
}

static int match_wlan(const struct net_ctx *net, const struct wlan_cache *cache, int *out_ifidx, int *out_si)
{
  unsigned best_metric = ~0U;
  int best_ifidx = -1;
  int best_si = -1;
  for (int i = 0; i < cache->count; i++) {
    if (wlan_better(net, (unsigned)cache->ifindices[i], &best_metric)) {
      best_ifidx = cache->ifindices[i];
      best_si = i;
    }
  }
  *out_ifidx = best_ifidx;
  *out_si = best_si;
  return best_ifidx >= 0 ? 0 : -1;
}

static void cache_ssid(struct net_ctx *net, const struct wlan_cache *cache, int si)
{
  if (cache->fresh && si >= 0 && cache->ssids[si][0])
    memcpy(net->wlan_ssid, cache->ssids[si], sizeof net->wlan_ssid);
}

void find_wlan_nlk(struct clock_state *ci)
{
  struct net_ctx *net = &ci->keep.net;
  struct wlan_cache *cache = &net->wcache;

  if (net->wcache_stale || cache->count == 0) {
    if (wlan_open_nlk(ci) < 0)
      return;
    nlk_find_wlan_all(&ci->keep.nlk, cache);
    net->wcache_stale = 0;
  }

  int best_ifidx, best_si;
  if (match_wlan(net, cache, &best_ifidx, &best_si) < 0)
    return;

  int slot = ensure_wlan_slot_by_idx(net, best_ifidx);
  if (slot < 0)
    return;

  net->wlan_idx = slot;
  cache_ssid(net, cache, best_si);
  cache->fresh = 0;
}
