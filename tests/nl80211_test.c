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

static struct nlattr *add_nla(int type, int paylen)
{
  unsigned char *p = (unsigned char *)rgh + GENL_HDRLEN;
  size_t off = 0;
  /* find first empty slot */
  while (off < sizeof rbuf - 64) {
    struct nlattr *a = (struct nlattr *)(p + off);
    if (a->nla_type == 0 && a->nla_len == 0)
      break;
    off += NLA_ALIGN(a->nla_len);
  }
  struct nlattr *a = (struct nlattr *)(p + off);
  a->nla_type = (unsigned short)type;
  a->nla_len = (unsigned short)(NLA_HDRLEN + paylen);
  rnh->nlmsg_len = (unsigned)(NLMSG_LENGTH(GENL_HDRLEN + off + NLA_ALIGN(a->nla_len)));
  return a;
}

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

static void set_ssid(const char *ssid)
{
  size_t sl = strlen(ssid);
  struct nlattr *a = add_nla(NL80211_ATTR_SSID, (int)sl);
  memcpy((char *)a + NLA_HDRLEN, ssid, sl);
}

static void set_station_info(unsigned short rate_100kbps, signed char dbm)
{
  unsigned char *p = (unsigned char *)rgh + GENL_HDRLEN;
  struct nlattr *si = (struct nlattr *)p;
  si->nla_type = NL80211_ATTR_STA_INFO;
  si->nla_len = NLA_HDRLEN + 12;        /* 4+8: header + bitrate attr */
  /* TX bitrate nested attr */
  struct nlattr *tx = (struct nlattr *)(p + NLA_HDRLEN);
  tx->nla_type = NL80211_STA_INFO_TX_BITRATE;
  tx->nla_len = NLA_HDRLEN + 6;
  struct nlattr *br = (struct nlattr *)((char *)tx + NLA_HDRLEN);
  br->nla_type = NL80211_RATE_INFO_BITRATE;
  br->nla_len = NLA_HDRLEN + 2;
  *(unsigned short *)((char *)br + NLA_HDRLEN) = rate_100kbps;
  /* Signal */
  struct nlattr *sg = (struct nlattr *)(p + NLA_HDRLEN + NLA_ALIGN(tx->nla_len));
  sg->nla_type = NL80211_STA_INFO_SIGNAL;
  sg->nla_len = NLA_HDRLEN + 1;
  *(signed char *)((char *)sg + NLA_HDRLEN) = dbm;
  si->nla_len = (unsigned short)(NLA_HDRLEN + (size_t)((char *)(sg + 1) - (char *)si));
  rnh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN + NLA_ALIGN(si->nla_len));
}

/* helper: make a DONE message */
static void mkdone(unsigned char *buf, size_t *len)
{
  struct nlmsghdr *dh = (struct nlmsghdr *)buf;
  dh->nlmsg_len = sizeof(struct nlmsghdr);
  dh->nlmsg_type = NLMSG_DONE;
  *len = dh->nlmsg_len;
}

static int test_nlk_ssid(void)
{
  mock_reset();
  mkresp(28, NL80211_CMD_NEW_INTERFACE);
  set_ssid("HomeNet");
  unsigned char resp[256];
  size_t rlen = rnh->nlmsg_len;
  memcpy(resp, rbuf, rlen);
  /* talk(): sendmsg(noop), recvmsg returns SSID response */
  mock_set_netlink(9, resp, rlen, NULL, 0);
  struct netlink_ctx nlk;
  memset(&nlk, 0, sizeof nlk);
  nlk.fd = 9;
  nlk.family = 28;
  nlk.ifindex = 3;
  char ssid[64] = "";
  int rc = nlk_wlan_ssid(&nlk, "wlan0", ssid, sizeof ssid);
  if (rc < 0)
    return 30;
  if (strcmp(ssid, "HomeNet") != 0)
    return 31;
  return 0;
}

static int test_nlk_station(void)
{
  mock_reset();
  /* First talk: do_nl80211 for GET_STATION */
  mkresp(28, NL80211_CMD_GET_STATION);
  unsigned char getsta_buf[256];
  size_t getsta_len = rnh->nlmsg_len;
  memcpy(getsta_buf, rbuf, getsta_len);
  /* Second talk: talk_dump for send_station_req */
  mkresp(28, NL80211_CMD_GET_STATION);
  set_station_info(8667, -48);
  unsigned char stresp_buf[256];
  size_t stresp_len = rnh->nlmsg_len;
  memcpy(stresp_buf, rbuf, stresp_len);
  /* DONE for recv_done_loop */
  unsigned char done_buf[32];
  size_t done_len;
  mkdone(done_buf, &done_len);
  /* Mock responses in order:
   * recvmsg 1: GET_STATION response (no STA_INFO, just header)
   * recvmsg 2: station info response
   * recvmsg 3: DONE (talk_dump → recv_done_loop)
   * recvmsg 4-N: drain (bounded loop of 4)
   * Extra recvmsg: none needed after drain
   */
  /* nlk_station_rate:
   * 1. ensure_resolved: nlk->family=28 >=0 → skip
   * 2. iface_to_idx: nlk->ifindex=0 → sys_if_nametoindex("wlan0") → mock_set_ifindex(5) → idx=5
   * 3. do_nl80211: talk (sendmsg + recvmsg) → gets getsta_buf
   * 4. query_station: send_station_req → talk_dump (talk=sendmsg+recvmsg, then recv_done_loop)
   *    - talk recvmsg → stresp_buf
   *    - recv_done_loop recvmsg → done_buf
   */
  mock_set_ifindex(5);
  mock_set_netlink(9, getsta_buf, getsta_len, stresp_buf, stresp_len);
  mock_add_nl_resp(done_buf, done_len);
  struct netlink_ctx nlk;
  memset(&nlk, 0, sizeof nlk);
  nlk.fd = 9;
  nlk.family = 28;
  nlk.ifindex = 0;
  int rx = 0, tx = 0, dbm = 0;
  int rc = nlk_station_rate(&nlk, "wlan0", &rx, &tx, &dbm);
  if (rc < 0)
    return 40;
  if (tx != 866)
    return 41;
  if (dbm != -48)
    return 42;
  return 0;
}

int main(void)
{
  int rc;
  rc = test_nlk_ssid();
  if (rc)
    return rc;
  rc = test_nlk_station();
  if (rc)
    return rc;
  return 0;
}
