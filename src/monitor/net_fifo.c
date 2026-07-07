#define _GNU_SOURCE
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include <linux/rtnetlink.h>    // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

static int is_virtual_kind(const char *kind)
{
  static const char *VTYPES[] = {
    "tun", "tap", "veth", "bridge", "bond", "dummy",
    "sit", "gre", "gretap", "vti", "vlan", "vxlan", "geneve"
  };
  if (!kind || !kind[0])
    return 0;
  for (size_t k = 0; k < sizeof VTYPES / sizeof VTYPES[0]; k++)
    if (strcmp(kind, VTYPES[k]) == 0)
      return 1;
  return 0;
}

static int in_links(const struct rtnl_ctx *r, int ifindex)
{
  for (int i = 0; i < r->link_count; i++)
    if (r->links[i].ifindex == ifindex)
      return 1;
  return 0;
}

static int find_link_idx(const struct rtnl_ctx *r, int ifindex)
{
  for (int i = 0; i < r->link_count; i++)
    if (r->links[i].ifindex == ifindex)
      return i;
  return -1;
}

static void name_idx_compact(struct rtnl_ctx *r, int i)
{
  int rem = r->name_idx_n - i - 1;
  if (rem > 0)
    memmove(&r->name_idx[i], &r->name_idx[i + 1], (size_t)rem * sizeof r->name_idx[0]);
  r->name_idx_n--;
  if (r->name_idx_cap > 64 && r->name_idx_n <= r->name_idx_cap / 2) {
    int nc = (int)(((unsigned)r->name_idx_cap / 2 + 63) & ~63U);
    struct name_idx *p = realloc(r->name_idx, (size_t)nc * sizeof *p);
    if (p) {
      r->name_idx = p;
      r->name_idx_cap = nc;
    }
  }
}

static void name_idx_clear_entry(struct rtnl_ctx *r, unsigned ifindex)
{
  for (int i = 0; i < r->name_idx_n; i++)
    if (r->name_idx[i].ifindex == ifindex) {
      name_idx_compact(r, i);
      return;
    }
}

static void wcache_remove_entry(struct wlan_cache *wcache, int ifindex)
{
  for (int i = 0; i < wcache->count; i++) {
    if (wcache->ifindices[i] != ifindex)
      continue;
    int rem = wcache->count - i - 1;
    if (rem > 0) {
      memmove(&wcache->ifindices[i], &wcache->ifindices[i + 1], (size_t)rem * sizeof wcache->ifindices[0]);
      memmove(&wcache->ssids[i], &wcache->ssids[i + 1], (size_t)rem * sizeof wcache->ssids[0]);
    }
    wcache->count--;
    return;
  }
}

static int in_link_idx(const struct net_ctx *c, int ifindex)
{
  for (unsigned i = 0; i < c->count; i++)
    if (c->ifindex[i] == ifindex)
      return 1;
  return 0;
}

static int in_gw_cache(const struct net_ctx *c, int ifindex)
{
  for (int i = 0; i < c->gw_n; i++)
    if (c->gw_idx[i] == ifindex)
      return 1;
  return 0;
}

static void proc_link_add(struct net_ctx *c, const struct rtnl_ctx *r, const struct mon_action *act,
                          int *acc_links_stale)
{
  if (in_links(r, act->arg.link.ifindex))
    return;
  if (is_virtual_kind(act->arg.link.kind) && c->has_phys)
    return;
  *acc_links_stale = 1;
  c->wcache_stale = 1;
}

static void adjust_link_idxs(struct net_ctx *c, int idx)
{
  for (unsigned j = 0; j < c->count && j < MAX_NET; j++)
    if (c->link_idx[j] > idx)
      c->link_idx[j]--;
}

static void rm_link_entry(struct rtnl_ctx *r, int idx)
{
  int rem = r->link_count - idx;
  memmove(r->links + idx, r->links + idx + 1, (size_t)rem * sizeof r->links[0]);
  r->link_count--;
}

static void proc_link_remove(struct net_ctx *c, struct rtnl_ctx *r, const struct mon_action *act, int *acc_reprimary)
{
  int idx = find_link_idx(r, act->arg.link.ifindex);
  if (idx < 0)
    return;
  rm_link_entry(r, idx);
  name_idx_clear_entry(r, (unsigned)act->arg.link.ifindex);
  wcache_remove_entry(&c->wcache, act->arg.link.ifindex);
  adjust_link_idxs(c, idx);
  if (is_virtual_kind(act->arg.link.kind) && c->has_phys)
    return;
  *acc_reprimary = 1;
}

static void proc_addr4(struct clock_state *ci, const struct mon_action *act)
{
  if (in_link_idx(&ci->keep.net, act->arg.addr.ifindex))
    ci->keep.rtnl_mon.addr4_changed = 1;
}

static void proc_addr6(struct clock_state *ci, const struct mon_action *act)
{
  if (in_link_idx(&ci->keep.net, act->arg.addr.ifindex))
    ci->keep.rtnl_mon.addr6_changed = 1;
}

static void proc_route_add(const struct mon_action *act, int *acc_needs_route)
{
  if (act->arg.route.table == RT_TABLE_MAIN && act->arg.route.dst_len == 0)
    *acc_needs_route = 1;
}

static void proc_route_remove(const struct net_ctx *c, const struct mon_action *act, int *acc_needs_route)
{
  if (act->arg.route.table == RT_TABLE_MAIN && act->arg.route.dst_len == 0
      && (in_gw_cache(c, act->arg.route.ifindex) || in_link_idx(c, act->arg.route.ifindex)))
    *acc_needs_route = 1;
}

struct pa_ctx {
  int links_stale;
  int needs_route;
  int reprimary;
};

static void process_link_actions(const struct mon_action *act, struct rtnl_ctx *r, struct net_ctx *c, struct pa_ctx *pa)
{
  switch (act->type) {
  case MON_LINK_ADD:
    proc_link_add(c, r, act, &pa->links_stale);
    return;
  case MON_LINK_REMOVE:
    proc_link_remove(c, r, act, &pa->reprimary);
    return;
  default:
    return;
  }
}

static void process_addr_actions(struct clock_state *ci, const struct mon_action *act)
{
  switch (act->type) {
  case MON_ADDR4_ADD:
  case MON_ADDR4_REMOVE:
    proc_addr4(ci, act);
    return;
  case MON_ADDR6_ADD:
  case MON_ADDR6_REMOVE:
    proc_addr6(ci, act);
    return;
  default:
    return;
  }
}

static void process_route_actions(const struct net_ctx *c, const struct mon_action *act, int *needs_route)
{
  switch (act->type) {
  case MON_ROUTE_ADD:
    proc_route_add(act, needs_route);
    return;
  case MON_ROUTE_REMOVE:
    proc_route_remove(c, act, needs_route);
    return;
  default:
    return;
  }
}

static void apply_pa(struct rtnl_ctx *r, struct net_ctx *c, const struct pa_ctx *pa)
{
  if (pa->links_stale)
    r->links_stale = 1;
  if (pa->needs_route)
    c->needs_route = 1;
  if (pa->reprimary)
    c->needs_reprimary = 1;
}

static int fifo_lock(struct mon_action_fifo *f)
{
  for (int i = 0; i < 10; i++) {
    if (pthread_mutex_trylock(&f->lock) == 0)
      return 0;
    nanosleep(&(struct timespec) {.tv_nsec = 10000000 }, NULL);
  }
  return -1;
}

static void drain_fifo(struct clock_state *ci, struct rtnl_ctx *r, struct net_ctx *c)
{
  struct mon_action_fifo *f = &ci->keep.rtnl_mon.fifo;
  if (!ci->keep.rtnl_mon.started || fifo_lock(f) != 0)
    return;
  struct pa_ctx pa = { 0 };
  for (size_t i = 0; i < f->count; i++) {
    const struct mon_action *act = &f->items[i];
    process_link_actions(act, r, c, &pa);
    process_addr_actions(ci, act);
    process_route_actions(c, act, &pa.needs_route);
  }
  f->count = 0;
  pthread_mutex_unlock(&f->lock);
  apply_pa(r, c, &pa);
}

static int poll_refresh(struct rtnl_ctx *r, struct net_ctx *c)
{
  int do_route = c->needs_route;
  int need_refresh = do_route | r->links_stale;
  if (need_refresh)
    rtnl_cache_links(r);
  if (do_route) {
    c->needs_route = 0;
    return 1;
  }
  return 0;
}

static int nics_changed(const struct net_ctx *c, const int old_idx[MAX_NET], int old_count)
{
  if ((int)c->count != old_count)
    return 1;
  for (int i = 0; i < (int)c->count; i++)
    if (c->ifindex[i] != old_idx[i])
      return 1;
  return 0;
}

static void refresh_primaries(struct clock_state *ci, struct rtnl_ctx *r, struct net_ctx *c)
{
  int old_idx[MAX_NET];
  memcpy(old_idx, c->ifindex, sizeof old_idx);
  int old_count = (int)c->count;
  if (poll_refresh(r, c))
    pick_primary(c, r, 0);
  if (c->needs_reprimary) {
    c->needs_reprimary = 0;
    pick_primary(c, r, 1);
  }
  if (nics_changed(c, old_idx, old_count)) {
    ci->keep.rtnl_mon.addr4_changed = 1;
    ci->keep.rtnl_mon.addr6_changed = 1;
  }
}

void get_net_info(struct clock_state *ci)
{
  ci->net_line[0] = 0;
  struct rtnl_ctx *r = &ci->keep.rtnl;
  struct net_ctx *c = &ci->keep.net;
  if (r->fd <= 0) {
    rtnl_open(r);
    c->needs_route = 1;
    r->links_stale = 1;
  }
  drain_fifo(ci, r, c);
  refresh_primaries(ci, r, c);
  if (c->count == 0)
    c->needs_route = 1;
  do_wlan(ci);
  read_net_dev(ci, r);
}
