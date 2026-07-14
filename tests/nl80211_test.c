#include "mock_syscall.h"
#include "monitor/monitor.h"
#include "monitor/monitor_int.h"
#include "monitor/widget.h"
#include <string.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <linux/netlink.h>      // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <linux/genetlink.h>    // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <linux/nl80211.h>      // cppcheck-suppress missingIncludeSystem -- musl include paths not known

static unsigned char rbuf[4096];
static struct nlmsghdr *rnh;
static struct genlmsghdr *rgh;

static void mkresp(unsigned short family, unsigned char cmd)
{
  memset(rbuf, 0, sizeof rbuf);
  rnh = (struct nlmsghdr *)rbuf;
  rgh = NLMSG_DATA(rnh);
  rnh->nlmsg_type = family;
  rgh->cmd = cmd;
  rgh->version = 1;
  rnh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
}

static void set_station_info(unsigned short rate_100kbps, signed char dbm)
{
  unsigned char *p = (unsigned char *)rgh + GENL_HDRLEN;
  struct nlattr *si = (struct nlattr *)p;
  si->nla_type = NL80211_ATTR_STA_INFO;
  si->nla_len = NLA_HDRLEN + 12;
  struct nlattr *tx = (struct nlattr *)(p + NLA_HDRLEN);
  tx->nla_type = NL80211_STA_INFO_TX_BITRATE;
  tx->nla_len = NLA_HDRLEN + 6;
  struct nlattr *br = (struct nlattr *)((char *)tx + NLA_HDRLEN);
  br->nla_type = NL80211_RATE_INFO_BITRATE;
  br->nla_len = NLA_HDRLEN + 2;
  *(unsigned short *)((char *)br + NLA_HDRLEN) = rate_100kbps;
  struct nlattr *sg = (struct nlattr *)(p + NLA_HDRLEN + NLA_ALIGN(tx->nla_len));
  sg->nla_type = NL80211_STA_INFO_SIGNAL;
  sg->nla_len = NLA_HDRLEN + 1;
  *(signed char *)((char *)sg + NLA_HDRLEN) = dbm;
  si->nla_len = (unsigned short)(NLA_HDRLEN + (size_t)((char *)(sg + 1) - (char *)si));
  rnh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN + NLA_ALIGN(si->nla_len));
}

static void set_bss_nested(const unsigned char *bssid, const char *ssid)
{
  unsigned char *p = (unsigned char *)rgh + GENL_HDRLEN;
  size_t off = 0;
  while (off < sizeof rbuf - 128) {
    struct nlattr *a = (struct nlattr *)(p + off);
    if (a->nla_type == 0 && a->nla_len == 0)
      break;
    off += NLA_ALIGN(a->nla_len);
  }
  struct nlattr *bss = (struct nlattr *)(p + off);
  bss->nla_type = NL80211_ATTR_BSS;
  unsigned char *bp = (unsigned char *)bss + NLA_HDRLEN;
  struct nlattr *bid = (struct nlattr *)bp;
  bid->nla_type = NL80211_BSS_BSSID;
  bid->nla_len = NLA_HDRLEN + 6;
  memcpy(bp + NLA_HDRLEN, bssid, 6);
  bp += NLA_ALIGN(bid->nla_len);
  struct nlattr *ie = (struct nlattr *)bp;
  ie->nla_type = NL80211_BSS_INFORMATION_ELEMENTS;
  size_t slen = strlen(ssid);
  ie->nla_len = (unsigned short)(NLA_HDRLEN + 2 + slen);
  bp += NLA_HDRLEN;
  bp[0] = 0;
  bp[1] = (unsigned char)slen;
  memcpy(bp + 2, ssid, slen);
  bss->nla_len = (unsigned short)(NLA_HDRLEN + NLA_ALIGN(bid->nla_len) + NLA_ALIGN(ie->nla_len));
  rnh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN + off + NLA_ALIGN(bss->nla_len));
}

static void mkdone(unsigned char *buf, size_t *len)
{
  struct nlmsghdr *dh = (struct nlmsghdr *)buf;
  dh->nlmsg_len = sizeof(struct nlmsghdr);
  dh->nlmsg_type = NLMSG_DONE;
  *len = dh->nlmsg_len;
}

static int test_nlk_scan_bss(void)
{
  mock_reset();
  static const unsigned char bssid[] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
  mkresp(28, NL80211_CMD_GET_SCAN);
  set_bss_nested(bssid, "HomeNet");
  unsigned char resp[512];
  size_t rlen = rnh->nlmsg_len;
  memcpy(resp, rbuf, rlen);
  mock_set_netlink(9, resp, rlen, NULL, 0);
  unsigned char done_buf[32];
  size_t done_len;
  mkdone(done_buf, &done_len);
  mock_add_nl_resp(done_buf, done_len);
  struct netlink_ctx nlk;
  memset(&nlk, 0, sizeof nlk);
  nlk.fd = 9;
  nlk.family = 28;
  nlk.ifindex = 3;
  mock_set_ifindex(5);
  struct net_ctx net;
  memset(&net, 0, sizeof net);
  int rc = nlk_scan_bss(&nlk, "wlan0", &net);
  if (rc < 0)
    return 30;
  if (memcmp(net.bss_mac, bssid, 6) != 0)
    return 31;
  if (strcmp(net.wlan_ssid, "HomeNet") != 0)
    return 32;
  return 0;
}

static int test_nlk_station(void)
{
  mock_reset();
  struct netlink_ctx nlk;
  memset(&nlk, 0, sizeof nlk);
  nlk.fd = 9;
  nlk.family = 28;
  nlk.ifindex = 0;
  struct net_ctx net;
  memset(&net, 0, sizeof net);
  int rc = nlk_station_rate(&nlk, "wlan0", &net);
  if (rc == 0)
    return 40;
  return 0;
}

static int test_nlk_station_targeted(void)
{
  mock_reset();
  static const unsigned char test_mac[] = { 0xf2, 0xdf, 0xba, 0x69, 0xe4, 0xaf };
  mkresp(28, NL80211_CMD_GET_STATION);
  set_station_info(5400, -55);
  unsigned char tgt_buf[256];
  size_t tgt_len = rnh->nlmsg_len;
  memcpy(tgt_buf, rbuf, tgt_len);
  mock_set_ifindex(5);
  mock_set_netlink(9, tgt_buf, tgt_len, NULL, 0);
  struct netlink_ctx nlk;
  memset(&nlk, 0, sizeof nlk);
  nlk.fd = 9;
  nlk.family = 28;
  nlk.ifindex = 0;
  struct net_ctx net;
  memset(&net, 0, sizeof net);
  memcpy(net.bss_mac, test_mac, 6);
  int rc = nlk_station_rate(&nlk, "wlan0", &net);
  if (rc < 0)
    return 50;
  if (net.wlan_tx_rate != 540)
    return 51;
  if (net.wlan_dbm != -55)
    return 52;
  if (memcmp(net.bss_mac, test_mac, 6) != 0)
    return 53;
  return 0;
}

int main(void)
{
  int rc;
  rc = test_nlk_scan_bss();
  if (rc)
    return rc;
  rc = test_nlk_station();
  if (rc)
    return rc;
  rc = test_nlk_station_targeted();
  if (rc)
    return rc;
  return 0;
}
