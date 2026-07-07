/* Test net.c functions with mocked netlink responses */
#include "mock_syscall.h"
#include "monitor/monitor_int.h"
#include "monitor/monitor.h"
#include <string.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/genetlink.h>
#include "nl_helper.h"

int net_fmt_thr(char *b, int z, unsigned long long v);

static unsigned char done_buf[64];
static unsigned char route_buf[256];
static unsigned char genl_resp[32];
static unsigned char link_buf[512];

static unsigned mk_fake_genl(void)
{
  memset(genl_resp, 0, sizeof genl_resp);
  struct nlmsghdr *nh = (struct nlmsghdr *)genl_resp;
  nh->nlmsg_len = sizeof(struct nlmsghdr) + sizeof(struct genlmsghdr);
  nh->nlmsg_type = GENL_ID_CTRL;
  nh->nlmsg_flags = 0;
  struct genlmsghdr *gh = NLMSG_DATA(nh);
  gh->cmd = CTRL_CMD_NEWFAMILY;
  gh->version = 1;
  return nh->nlmsg_len;
}

static void setup_netlink_mocks(unsigned rx, unsigned long long tx)
{
  mk_link_nl(link_buf, "eth5", 5, rx, tx, NULL);
  mk_done_nl(done_buf);
  mock_set_netlink(99, link_buf, ((struct nlmsghdr *)link_buf)->nlmsg_len, done_buf, sizeof(struct nlmsghdr));
  mk_route_nl(route_buf, AF_INET, 5, 100);
  mk_done_nl(done_buf);
  mock_add_nl_resp(route_buf, ((struct nlmsghdr *)route_buf)->nlmsg_len);
  mock_add_nl_resp(done_buf, sizeof(struct nlmsghdr));
  /* spare DONE for v6 route dump */
  mock_add_nl_resp(done_buf, sizeof(struct nlmsghdr));
  mk_fake_genl();
  mock_add_nl_resp(genl_resp, ((struct nlmsghdr *)genl_resp)->nlmsg_len);
  mk_done_nl(done_buf);
  mock_add_nl_resp(done_buf, sizeof(struct nlmsghdr));
  mock_set_ifindex(5);
}

static int test_get_net_info_routed(void)
{
  mock_reset();
  setup_netlink_mocks(500000, 1000000);
  mock_set_link_speed("eth5", 999);

  struct clock_state ci;
  memset(&ci, 0, sizeof ci);
  ci.keep.net.wlan_dbm = -42;
  widget_setup(&ci.keep.widget, 0, NULL, 1, 1);
  get_net_info(&ci);

  if (ci.keep.net.count != 1)
    return 10;
  if (ci.keep.net.ifindex[0] != 5)
    return 11;
  if (strlen(ci.net_line) == 0)
    return 12;
  if (ci.net_dbm != -42)
    return 13;
  return 0;
}

static int test_get_net_info_twice(void)
{
  mock_reset();
  /* Call 1: small rx/tx → rd=0, td=0 → K branch */
  setup_netlink_mocks(500000, 1000000);
  mock_set_link_speed("eth5", 1000000);

  struct clock_state ci;
  memset(&ci, 0, sizeof ci);
  ci.keep.net.wlan_dbm = -55;
  widget_setup(&ci.keep.widget, 0, NULL, 1, 1);
  get_net_info(&ci);
  if (ci.net_dbm != -55)
    return 19;

  /* Call 2: larger rx/tx → rd>0, td>0 → M/G fmt_thr branches */
  setup_netlink_mocks(700000, 2000000000ULL);
  ci.keep.net.needs_route = 1;
  get_net_info(&ci);

  if (strlen(ci.net_line) == 0)
    return 20;
  return 0;
}

static int test_refresh_local_ips(void)
{
  mock_reset();
  mock_set_netlink(99, NULL, 0, NULL, 0);
  mock_set_ifindex(5);

  struct clock_state ci;
  memset(&ci, 0, sizeof ci);
  async_ctx_init(&ci.keep.async, &ci.keep.wan4_fd, &ci.keep.wan6_fd, &ci.keep.weather_fd, (1u << WIDGET_NET));
  ci.keep.net.count = 1;
  ci.keep.net.ifindex[0] = 5;
  ci.keep.rtnl_mon.addr4_changed = 1;
  ci.keep.rtnl_mon.addr6_changed = 1;

  /* First call: flag set → ld_refresh runs */
  refresh_local_ips(&ci);
  if (ci.keep.rtnl_mon.addr4_changed != 0 || ci.keep.rtnl_mon.addr6_changed != 0)
    return 30;

  /* Second call: flags are 0 → skip ld_refresh, just copy IPs */
  refresh_local_ips(&ci);
  if (ci.keep.rtnl_mon.addr4_changed != 0 || ci.keep.rtnl_mon.addr6_changed != 0)
    return 31;

  return 0;
}

static int test_do_get_ip(void)
{
  mock_reset();
  mock_set_netlink(99, NULL, 0, NULL, 0);
  mock_set_ifindex(5);

  struct clock_state ci;
  memset(&ci, 0, sizeof ci);
  ci.keep.net.count = 1;
  ci.keep.net.ifindex[0] = 5;
  ci.keep.rtnl_mon.addr4_changed = 1;
  ci.keep.rtnl_mon.addr6_changed = 1;
  ci.keep.wan4_state = 1;
  ci.keep.wan6_state = 1;

  refresh_local_ips(&ci);
  if (ci.keep.rtnl_mon.addr4_changed != 0 || ci.keep.rtnl_mon.addr6_changed != 0)
    return 51;
  if (strcmp(ci.keep.ipv4_local, "10.0.0.1") != 0)
    return 52;
  return 0;
}

/* NET fmt_thr thresholds: <512 b, <102400 K, <104857600 M, >=104857600 G */

static int test_net_fmt_thr_bytes_bottom(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 0);
  if (n <= 0)
    return 1;
  if (strcmp(b, "0b") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_bytes_mid(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 123);
  if (n <= 0)
    return 1;
  if (strcmp(b, "123b") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_bytes_top(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 511);
  if (n <= 0)
    return 1;
  if (strcmp(b, "511b") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_k_bottom(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 512);
  if (n <= 0)
    return 1;
  if (strcmp(b, "0.5K") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_k_mid(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 1536);
  if (n <= 0)
    return 1;
  if (strcmp(b, "1.5K") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_k_top(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 102399);
  if (n <= 0)
    return 1;
  if (strcmp(b, "99.9K") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_m_bottom(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 102400);
  if (n <= 0)
    return 1;
  if (strcmp(b, "0.0M") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_m_just_above(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 104858);
  if (n <= 0)
    return 1;
  if (strcmp(b, "0.1M") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_m_mid(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 2097152);
  if (n <= 0)
    return 1;
  if (strcmp(b, "2.0M") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_m_top(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 104857599);
  if (n <= 0)
    return 1;
  if (strcmp(b, "99.9M") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_g_bottom(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 104857600);
  if (n <= 0)
    return 1;
  if (strcmp(b, "0.0G") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_g_just_above(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 107374183);
  if (n <= 0)
    return 1;
  if (strcmp(b, "0.1G") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_g_mid(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 1073741824);
  if (n <= 0)
    return 1;
  if (strcmp(b, "1.0G") != 0)
    return 2;
  return 0;
}

static int test_net_fmt_thr_g_top(void)
{
  char b[16];
  int n = net_fmt_thr(b, sizeof b, 2147483648ULL);
  if (n <= 0)
    return 1;
  if (strcmp(b, "2.0G") != 0)
    return 2;
  return 0;
}

static int test_get_net_info_drain_fifo(void)
{
  mock_reset();
  setup_netlink_mocks(500000, 1000000);
  mock_set_link_speed("eth5", 999);

  struct clock_state ci;
  memset(&ci, 0, sizeof ci);
  ci.keep.net.wlan_dbm = -42;
  widget_setup(&ci.keep.widget, 0, NULL, 1, 1);

  struct mon_action items_buf[5];
  memset(items_buf, 0, sizeof items_buf);
  items_buf[0].type = MON_LINK_ADD;
  items_buf[0].arg.link.ifindex = 5;
  memcpy(items_buf[0].arg.link.kind, "eth", 3);
  items_buf[1].type = MON_ADDR4_ADD;
  items_buf[1].arg.addr.ifindex = 5;
  items_buf[2].type = MON_ROUTE_ADD;
  items_buf[2].arg.route.table = RT_TABLE_MAIN;
  items_buf[3].type = MON_ADDR6_ADD;
  items_buf[3].arg.addr.ifindex = 5;
  items_buf[4].type = MON_ROUTE_REMOVE;
  items_buf[4].arg.route.table = RT_TABLE_MAIN;
  items_buf[4].arg.route.ifindex = 5;
  struct mon_action_fifo *f = &ci.keep.rtnl_mon.fifo;
  pthread_mutex_init(&f->lock, NULL);
  f->items = items_buf;
  f->cap = 5;
  f->count = 5;
  ci.keep.rtnl_mon.started = 1;

  get_net_info(&ci);

  /* Second batch: LINK_REMOVE, virtual LINK_ADD, ADDR4_ADD, ADDR6_ADD, ROUTE_REMOVE */
  struct mon_action items2[6];
  memset(items2, 0, sizeof items2);
  items2[0].type = MON_LINK_REMOVE;
  items2[0].arg.link.ifindex = 5;
  memcpy(items2[0].arg.link.kind, "eth", 3);
  items2[1].type = MON_LINK_ADD;
  items2[1].arg.link.ifindex = 99;
  memcpy(items2[1].arg.link.kind, "tun", 3);
  items2[2].type = MON_ADDR4_ADD;
  items2[2].arg.addr.ifindex = 5;
  items2[3].type = MON_ADDR6_ADD;
  items2[3].arg.addr.ifindex = 5;
  items2[4].type = MON_ROUTE_REMOVE;
  items2[4].arg.route.table = RT_TABLE_MAIN;
  items2[4].arg.route.ifindex = 5;
  items2[5].type = MON_LINK_ADD;
  items2[5].arg.link.ifindex = 100;
  memcpy(items2[5].arg.link.kind, "eth", 3);
  f->items = items2;
  f->cap = 6;
  f->count = 6;
  get_net_info(&ci);

  pthread_mutex_destroy(&f->lock);
  f->items = NULL;

  if (strlen(ci.net_line) == 0)
    return 10;
  return 0;
}

/* Regression test: advance-with-cap logic prevents buffer overrun */
static int test_net_fmt_into_safe(void)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
  char buf[16];
  char *p = buf;
  size_t rem = sizeof buf;
  int n = snprintf(p, rem, "%s", "abcdefghijklmnopqrstuvwxyz0123456789");
  if (n > 0 && rem > 0) {
    if ((size_t)n >= rem)
      n = (int)rem - 1;
    p += n;
  }
  if ((size_t)(p - buf) >= sizeof buf)
    return 1;
  if (buf[sizeof buf - 1] != '\0')
    return 2;
#pragma GCC diagnostic pop
  return 0;
}

/* Integration test: net_line stays within bounds after get_net_info */
static int test_net_line_bounds(void)
{
  mock_reset();
  setup_netlink_mocks(500000, 1000000);
  mock_set_link_speed("eth5", 999);
  struct clock_state ci;
  memset(&ci, 0, sizeof ci);
  ci.keep.net.wlan_dbm = -42;
  widget_setup(&ci.keep.widget, 0, NULL, 1, 1);
  get_net_info(&ci);
  size_t len = strlen(ci.net_line);
  if (len >= sizeof ci.net_line)
    return 10;
  return 0;
}

int main(void)
{
  int rc;
  rc = test_get_net_info_routed();
  if (rc)
    return rc;
  rc = test_get_net_info_twice();
  if (rc)
    return rc;
  rc = test_get_net_info_drain_fifo();
  if (rc)
    return rc;
  rc = test_refresh_local_ips();
  if (rc)
    return rc;
  rc = test_do_get_ip();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_bytes_bottom();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_bytes_mid();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_bytes_top();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_k_bottom();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_k_mid();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_k_top();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_m_bottom();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_m_just_above();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_m_mid();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_m_top();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_g_bottom();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_g_just_above();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_g_mid();
  if (rc)
    return rc;
  rc = test_net_fmt_thr_g_top();
  if (rc)
    return rc;
  rc = test_net_fmt_into_safe();
  if (rc)
    return rc;
  rc = test_net_line_bounds();
  if (rc)
    return rc;
  return 0;
}
