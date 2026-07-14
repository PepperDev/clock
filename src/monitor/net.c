#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <netinet/in.h>         // cppcheck-suppress missingIncludeSystem
#include <arpa/inet.h>          // cppcheck-suppress missingIncludeSystem
#include <sys/ioctl.h>          // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <net/if.h>             // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

static const char *VTYPES[] = {
  "tun", "tap", "veth", "bridge", "bond", "dummy",
  "sit", "gre", "gretap", "vti", "vlan", "vxlan", "geneve"
};

int is_virtual_kind(const char *kind)
{
  if (!kind || !*kind)
    return 0;
  for (size_t k = 0; k < sizeof VTYPES / sizeof VTYPES[0]; k++)
    if (strcmp(kind, VTYPES[k]) == 0)
      return 1;
  return 0;
}

int get_dgram_fd(struct net_ctx *n)
{
  if (n->dgram_fd <= 0)
    n->dgram_fd = sys_socket(AF_INET, SOCK_DGRAM, 0);
  return n->dgram_fd;
}

static void do_get_ip(struct clock_state *ci, const char *iface)
{
  int fd = get_dgram_fd(&ci->keep.net);
  if (fd <= 0)
    return;
  struct ifreq req;
  strncpy(req.ifr_name, iface, sizeof req.ifr_name - 1);
  if (sys_ioctl(fd, SIOCGIFADDR, &req) == 0)
    inet_ntop(AF_INET, &((struct sockaddr_in *)&req.ifr_addr)->sin_addr, ci->keep.ipv4_local,
              sizeof ci->keep.ipv4_local);
}

static void cancel_wan_io(struct clock_state *ci)
{
  struct cpu_keep *k = &ci->keep;
  struct async_ctx *ctx = &k->async;
  dns_cancel(wan_dns_slot(ctx, 0));
  dns_cancel(wan_dns_slot(ctx, 1));
  http_result_cancel(&ctx->wan4_result);
  http_result_cancel(&ctx->wan6_result);
  io_cancel_all(&ctx->ioc, CANCEL_WAN);
  k->wan4.try = 0;
  k->wan6.try = 0;
}

static void refetch_wan(struct clock_state *ci)
{
  cancel_wan_io(ci);
  wan_dns_start(ci, 0);
  wan_dns_start(ci, 1);
}

static void refresh_ssid(struct clock_state *ci)
{
  if (ci->keep.net.wlan_idx >= 0 && ci->keep.nlk.fd > 0) {
    const char *name = name_idx_by_idx(&ci->keep.rtnl, (unsigned)ci->keep.net.ifindex[ci->keep.net.wlan_idx]);
    if (name)
      nlk_scan_bss(&ci->keep.nlk, name, &ci->keep.net);
  }
}

static int ip_cmp(const char p4[INET_ADDRSTRLEN], const char p6[INET6_ADDRSTRLEN], const struct clock_state *ci)
{
  return (p4[0] && strcmp(p4, ci->keep.ipv4_local)) || (p6[0] && strcmp(p6, ci->keep.ipv6_local));
}

static int ips_changed(struct clock_state *ci)
{
  char p4[INET_ADDRSTRLEN], p6[INET6_ADDRSTRLEN];
  memcpy(p4, ci->keep.ipv4_local, sizeof p4);
  memcpy(p6, ci->keep.ipv6_local, sizeof p6);
  if (ci->keep.net.count > 0) {
    const char *name = name_idx_by_idx(&ci->keep.rtnl, (unsigned)ci->keep.net.ifindex[0]);
    if (name)
      do_get_ip(ci, name);
  }
  ci->keep.rtnl.v6_cached = 0;
  get_local_ip6(ci);
  memcpy(ci->keep.ipv6_local, ci->local_ip6, sizeof ci->keep.ipv6_local);
  return ip_cmp(p4, p6, ci);
}

static void ld_refresh(struct clock_state *ci)
{
  if (ips_changed(ci) && !ci->keep.wan4_state)
    refetch_wan(ci);
  refresh_ssid(ci);
}

void refresh_local_ips(struct clock_state *ci)
{
  int a4 = ci->keep.rtnl_mon.addr4_changed;
  int a6 = ci->keep.rtnl_mon.addr6_changed;
  ci->keep.rtnl_mon.addr4_changed = 0;
  ci->keep.rtnl_mon.addr6_changed = 0;
  if (a4 || a6)
    ld_refresh(ci);
  memcpy(ci->local_ip, ci->keep.ipv4_local, sizeof ci->local_ip);
  memcpy(ci->local_ip6, ci->keep.ipv6_local, sizeof ci->local_ip6);
}
