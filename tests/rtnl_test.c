/* Test rtnetlink functions with realistic mock messages */
#define _GNU_SOURCE
#include "mock_syscall.h"
#include "monitor/monitor_int.h"
#include "monitor/widget.h"
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <linux/netlink.h>      // cppcheck-suppress missingIncludeSystem
#include <linux/rtnetlink.h>    // cppcheck-suppress missingIncludeSystem
#include <linux/if_link.h>      // cppcheck-suppress missingIncludeSystem
#include <arpa/inet.h>          // cppcheck-suppress missingIncludeSystem
#include "nl_helper.h"

static unsigned char done_buf[64];

static int test_rtnl_open(void)
{
  mock_reset();
  mock_set_netlink(99, NULL, 0, NULL, 0);
  struct rtnl_ctx r;
  if (rtnl_open(&r) < 0)
    return 10;
  if (r.fd != 99)
    return 11;
  return 0;
}

static int test_rtnl_default_v4_found(void)
{
  mock_reset();
  unsigned char rbuf[256];
  unsigned int rlen = mk_route_nl(rbuf, AF_INET, 7, 100);
  mock_set_ifindex(7);
  mock_set_ifname(7, "eth7");
  mk_done_nl(done_buf);
  mock_set_netlink(99, rbuf, rlen, done_buf, sizeof(struct nlmsghdr));
  struct rtnl_ctx r;
  memset(&r, 0, sizeof r);
  if (rtnl_open(&r) < 0)
    return 20;
  char out[24] = "";
  int gw[2] = { 0 };
  int gw_metric[2] = { 0 };
  struct gw_track gt = {.idx = gw,.metric = gw_metric,.cap = 2,.n = 0 };
  if (rtnl_default_v4_idx(&r, out, sizeof out, &gt) < 0)
    return 21;
  if (strcmp(out, "eth7") != 0)
    return 22;
  if (gt.n != 1 || gw[0] != 7)
    return 23;
  return 0;
}

static int test_rtnl_read_dev(void)
{
  mock_reset();
  mock_set_ifname(5, "eth0");
  mock_set_ifindex(5);
  unsigned char rbuf[512];
  unsigned int rlen = mk_link_nl(rbuf, "eth0", 5, 1000000, 2000000, NULL);
  mk_done_nl(done_buf);
  mock_set_netlink(99, rbuf, rlen, done_buf, sizeof(struct nlmsghdr));
  struct widget_ctx wctx;
  widget_setup(&wctx, 0, NULL);
  struct net_ctx nc;
  memset(&nc, 0, sizeof nc);
  nc.count = 1;
  nc.ifindex[0] = 5;
  nc.rp[0] = 500000;
  nc.tp[0] = 1000000;
  struct rtnl_ctx r;
  memset(&r, 0, sizeof r);
  if (rtnl_open(&r) < 0)
    return 30;
  unsigned long long rd[2] = { 0 }, td[2] = { 0 };
  rtnl_read_dev(&r, &nc, rd, td);
  if (rd[0] != 500000 || td[0] != 1000000)
    return 31;
  return 0;
}

static int test_rtnl_find_addr6(void)
{
  mock_reset();
  unsigned char rbuf[256];
  mk_addr6_nl(rbuf, "2001:db8::1", 7);
  mk_done_nl(done_buf);
  mock_set_ifindex(7);
  mock_set_netlink(99, rbuf, ((struct nlmsghdr *)rbuf)->nlmsg_len, done_buf, sizeof(struct nlmsghdr));
  struct rtnl_ctx r;
  memset(&r, 0, sizeof r);
  if (rtnl_open(&r) < 0)
    return 50;
  char ip6[64] = "";
  if (rtnl_find_addr6(&r, "eth0", ip6, sizeof ip6) < 0)
    return 51;
  if (strcmp(ip6, "2001:db8::1") != 0)
    return 52;
  return 0;
}

static int test_open_monitor_success(void)
{
  mock_reset();
  mock_set_netlink(42, NULL, 0, NULL, 0);
  struct rtnl_mon_ctx m;
  memset(&m, 0, sizeof m);
  if (rtnl_open_monitor(&m) < 0)
    return 60;
  if (m.fd != 42)
    return 61;
  if (m.started)
    return 62;
  rtnl_monitor_stop(&m);
  return 0;
}

static int test_open_monitor_fail(void)
{
  mock_reset();
  mock_set_netlink(-1, NULL, 0, NULL, 0);
  struct rtnl_mon_ctx m;
  memset(&m, 0, sizeof m);
  if (rtnl_open_monitor(&m) >= 0)
    return 70;
  return 0;
}

static int test_monitor_start_stop(void)
{
  mock_reset();
  mock_set_netlink(42, NULL, 0, NULL, 0);
  struct rtnl_mon_ctx m;
  memset(&m, 0, sizeof m);
  if (rtnl_open_monitor(&m) < 0)
    return 80;
  if (rtnl_monitor_start(&m) < 0)
    return 81;
  if (!m.started)
    return 82;
  rtnl_monitor_stop(&m);
  if (m.started)
    return 83;
  return 0;
}

static int test_monitor_recv_msg(void)
{
  mock_reset();
  mock_set_ifindex(5);
  mock_set_ifname(5, "eth0");
  unsigned char rbuf[256];
  unsigned int rlen = mk_addr6_nl(rbuf, "2001:db8::1", 5);
  unsigned char done[64];
  unsigned int dlen = mk_done_nl(done);
  mock_set_netlink(42, rbuf, rlen, done, dlen);
  mock_add_nl_resp(NULL, 0);
  struct rtnl_mon_ctx m;
  memset(&m, 0, sizeof m);
  if (rtnl_open_monitor(&m) < 0)
    return 90;
  mock_syscall_real_threads = 1;
  if (rtnl_monitor_start(&m) < 0)
    return 91;
  int found = 0;
  for (int i = 0; i < 200; i++) {
    pthread_mutex_lock(&m.fifo.lock);
    if (m.fifo.count > 0) {
      for (size_t j = 0; j < m.fifo.count; j++) {
        if (m.fifo.items[j].type == MON_ADDR6_ADD && m.fifo.items[j].arg.addr.ifindex == 5) {
          found = 1;
          break;
        }
      }
      m.fifo.count = 0;
    }
    pthread_mutex_unlock(&m.fifo.lock);
    if (found)
      break;
    struct timespec ts = {.tv_nsec = 500000 };
    nanosleep(&ts, NULL);
  }
  rtnl_monitor_stop(&m);
  mock_syscall_real_threads = 0;
  if (!found)
    return 92;
  return 0;
}

int main(void)
{
  int rc;
  rc = test_rtnl_open();
  if (rc)
    return rc;
  rc = test_rtnl_default_v4_found();
  if (rc)
    return rc;
  rc = test_rtnl_read_dev();
  if (rc)
    return rc;
  rc = test_rtnl_find_addr6();
  if (rc)
    return rc;
  rc = test_open_monitor_success();
  if (rc)
    return rc;
  rc = test_open_monitor_fail();
  if (rc)
    return rc;
  rc = test_monitor_start_stop();
  if (rc)
    return rc;
  rc = test_monitor_recv_msg();
  if (rc)
    return rc;
  return 0;
}
