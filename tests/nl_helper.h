/* Shared netlink message builders for tests */
#ifndef NL_HELPER_H
#define NL_HELPER_H
#include <arpa/inet.h>          // cppcheck-suppress missingIncludeSystem

static inline unsigned mk_done_nl(unsigned char *buf)
{
  struct nlmsghdr *nh = (struct nlmsghdr *)buf;
  memset(buf, 0, sizeof(struct nlmsghdr));
  nh->nlmsg_len = sizeof(struct nlmsghdr);
  nh->nlmsg_type = NLMSG_DONE;
  nh->nlmsg_flags = NLM_F_MULTI;
  return sizeof(struct nlmsghdr);
}

static inline unsigned mk_route_nl(unsigned char *buf, int family, int oif, unsigned metric)
{
  struct nlmsghdr *nh = (struct nlmsghdr *)buf;
  struct rtmsg *r = (struct rtmsg *)NLMSG_DATA(nh);
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg) + RTA_SPACE(4) + RTA_SPACE(4));
  nh->nlmsg_type = RTM_NEWROUTE;
  nh->nlmsg_flags = NLM_F_MULTI;
  r->rtm_family = (unsigned char)family;
  r->rtm_dst_len = 0;
  r->rtm_table = RT_TABLE_MAIN;
  r->rtm_protocol = RTPROT_STATIC;
  struct rtattr *rta_oif = RTM_RTA(r);
  rta_oif->rta_type = RTA_OIF;
  rta_oif->rta_len = RTA_SPACE(4);
  *(int *)RTA_DATA(rta_oif) = oif;
  struct rtattr *rta_prio = (struct rtattr *)((char *)rta_oif + RTA_SPACE(4));
  rta_prio->rta_type = RTA_PRIORITY;
  rta_prio->rta_len = RTA_SPACE(4);
  *(unsigned *)RTA_DATA(rta_prio) = metric;
  return nh->nlmsg_len;
}

static inline unsigned mk_link_nl(unsigned char *buf, const char *name, int ifindex,
                                  unsigned long long rx, unsigned long long tx, const char *kind)
{
  struct nlmsghdr *nh = (struct nlmsghdr *)buf;
  struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nh);
  size_t nlen = strlen(name) + 1;
  size_t st64sz = sizeof(struct rtnl_link_stats64);
  ifi->ifi_index = ifindex;
  struct rtattr *rta_name = IFLA_RTA(ifi);
  rta_name->rta_type = IFLA_IFNAME;
  rta_name->rta_len = RTA_SPACE(nlen);
  memcpy(RTA_DATA(rta_name), name, nlen);
  struct rtattr *rta_st = (struct rtattr *)((char *)rta_name + RTA_SPACE(nlen));
  rta_st->rta_type = IFLA_STATS64;
  rta_st->rta_len = RTA_SPACE(st64sz);
  struct rtnl_link_stats64 *st = (struct rtnl_link_stats64 *)RTA_DATA(rta_st);
  st->rx_bytes = rx;
  st->tx_bytes = tx;
  unsigned char *end = (unsigned char *)rta_st + RTA_SPACE(st64sz);
  if (kind) {
    struct rtattr *rta_li = (struct rtattr *)end;
    rta_li->rta_type = IFLA_LINKINFO;
    unsigned char *lp = (unsigned char *)rta_li + NLA_HDRLEN;
    struct rtattr *rta_kind = (struct rtattr *)lp;
    size_t klen = strlen(kind) + 1;
    rta_kind->rta_type = IFLA_INFO_KIND;
    rta_kind->rta_len = RTA_LENGTH(klen);
    memcpy(RTA_DATA(rta_kind), kind, klen);
    rta_li->rta_len = (unsigned short)(NLA_HDRLEN + RTA_ALIGN(rta_kind->rta_len));
    end = (unsigned char *)rta_li + RTA_ALIGN(rta_li->rta_len);
  }
  nh->nlmsg_len = (unsigned)(end - (unsigned char *)nh);
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_MULTI;
  return nh->nlmsg_len;
}

static inline unsigned mk_addr6_nl(unsigned char *buf, const char *str, unsigned ifindex)
{
  struct nlmsghdr *nh = (struct nlmsghdr *)buf;
  struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nh);
  struct in6_addr a6;
  inet_pton(AF_INET6, str, &a6);
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg) + RTA_SPACE(sizeof a6));
  nh->nlmsg_type = RTM_NEWADDR;
  nh->nlmsg_flags = NLM_F_MULTI;
  ifa->ifa_family = AF_INET6;
  ifa->ifa_index = ifindex;
  struct rtattr *rta = IFA_RTA(ifa);
  rta->rta_type = IFA_ADDRESS;
  rta->rta_len = RTA_SPACE(sizeof a6);
  memcpy(RTA_DATA(rta), &a6, sizeof a6);
  return nh->nlmsg_len;
}

#endif
