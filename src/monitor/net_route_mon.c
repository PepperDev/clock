#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <errno.h>              // cppcheck-suppress missingIncludeSystem
#include <poll.h>               // cppcheck-suppress missingIncludeSystem
#include <sys/ioctl.h>          // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <linux/netlink.h>      // cppcheck-suppress missingIncludeSystem
#include <linux/rtnetlink.h>    // cppcheck-suppress missingIncludeSystem
#include <linux/if_link.h>      // cppcheck-suppress missingIncludeSystem
#include <linux/if.h>           // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

static enum mon_action_type addr_type_for(unsigned short family, unsigned short nlmsg_type)
{
  if (family != AF_INET && family != AF_INET6)
    return MON_NONE;
  if (nlmsg_type != RTM_NEWADDR)
    return (family == AF_INET) ? MON_ADDR4_REMOVE : MON_ADDR6_REMOVE;
  return (family == AF_INET) ? MON_ADDR4_ADD : MON_ADDR6_ADD;
}

static void mon_process_link(struct rtnl_mon_ctx *m, const struct nlmsghdr *h)
{
  const struct ifinfomsg *ifi = (const struct ifinfomsg *)NLMSG_DATA(h);
  if (ifi->ifi_flags & IFF_LOOPBACK)
    return;
  push_link_action(m, h, MON_LINK_ADD);
}

static void mon_process_addr(struct rtnl_mon_ctx *m, const struct nlmsghdr *h, unsigned short t)
{
  const struct ifaddrmsg *ifa = (const struct ifaddrmsg *)NLMSG_DATA(h);
  enum mon_action_type at = addr_type_for(ifa->ifa_family, t);
  if (at == MON_NONE)
    return;
  push_addr_action(m, h, at);
}

static void mon_handle_nlmsg(struct rtnl_mon_ctx *m, const struct nlmsghdr *h)
{
  unsigned short t = h->nlmsg_type;
  if (t <= RTM_DELLINK) {
    if (t == RTM_NEWLINK)
      mon_process_link(m, h);
    else
      push_link_action(m, h, MON_LINK_REMOVE);
    return;
  }
  if (t <= RTM_DELADDR) {
    mon_process_addr(m, h, t);
    return;
  }
  if (t <= RTM_DELROUTE)
    push_route_action(m, h, t == RTM_NEWROUTE ? MON_ROUTE_ADD : MON_ROUTE_REMOVE);
}

static void mon_process_msg(struct rtnl_mon_ctx *m, unsigned char *buf, size_t n)
{
  size_t rem = n;
  for (struct nlmsghdr * h = (struct nlmsghdr *)buf; NLMSG_OK(h, rem); h = NLMSG_NEXT(h, rem))
    mon_handle_nlmsg(m, h);
}

static int ensure_buf(unsigned char **buf, size_t *cap, int avail)
{
  if (avail < 0)
    return -1;
  if ((size_t)avail <= *cap)
    return 0;
  size_t ncap = *cap;
  if (ncap == 0)
    ncap = NLBUF;
  while (ncap < (size_t)avail)
    ncap *= 2;
  ncap = (ncap + 63) & ~(size_t)63;
  unsigned char *nb = realloc(*buf, ncap);
  if (!nb)
    return -1;
  *buf = nb;
  *cap = ncap;
  return 0;
}

static int mon_poll_with_stop(const struct rtnl_mon_ctx *m)
{
  struct pollfd pfd = {.fd = m->fd,.events = POLLIN };
  int pr;
  do
    pr = sys_poll(&pfd, 1, POLL_WAIT_MS);
  while (pr < 0 && errno == EINTR && m->fd >= 0);
  return pr;
}

static ssize_t mon_do_recv(int fd, unsigned char *buf, size_t len)
{
  struct iovec iov = {.iov_base = buf,.iov_len = len };
  struct msghdr msg = {.msg_iov = &iov,.msg_iovlen = 1 };
  return sys_recvmsg(fd, &msg, 0);
}

static int mon_recv(struct rtnl_mon_ctx *m, unsigned char **buf, size_t *cap)
{
  int pr = mon_poll_with_stop(m);
  if (pr < 0)
    return -1;
  if (pr == 0)
    return 0;
  size_t want = *cap ? *cap : NLBUF;
  if (ensure_buf(buf, cap, (int)want) < 0)
    return -1;
  ssize_t n = mon_do_recv(m->fd, *buf, *cap);
  if (n < 0)
    return -1;
  mon_process_msg(m, *buf, (size_t)n);
  return 0;
}

static int mon_thread_recv(struct rtnl_mon_ctx *m, unsigned char **buf, size_t *cap)
{
  int r = mon_recv(m, buf, cap);
  if (r < 0) {
    if (errno == EINTR) {
      if (m->fd < 0)
        return -1;
      return 1;
    }
    return -1;
  }
  return 0;
}

static int mon_thread_tick(struct rtnl_mon_ctx *m, unsigned char **buf, size_t *cap)
{
  if (m->stop || m->fd < 0)
    return -1;
  int rc = mon_thread_recv(m, buf, cap);
  if (rc == 1)
    return 0;
  return (rc < 0) ? -1 : 1;
}

static void *rtnl_mon_thread(void *arg)
{
  struct rtnl_mon_ctx *m = arg;
  size_t cap = NLBUF;
  unsigned char *buf = malloc(cap);
  if (!buf)
    return NULL;
  while (mon_thread_tick(m, &buf, &cap) >= 0) ;
  free(buf);
  return NULL;
}

static int mon_open_sock(void)
{
  int fd = sys_socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (fd < 0)
    return -1;
  struct sockaddr_nl sa = {
    .nl_family = AF_NETLINK,
    .nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE,
  };
  if (sys_bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
    sys_close(fd);
    return -1;
  }
  return fd;
}

int rtnl_open_monitor(struct rtnl_mon_ctx *m)
{
  m->fd = -1;
  m->started = 0;
  m->addr4_changed = 0;
  m->addr6_changed = 0;
  m->stop = 0;
  memset(&m->fifo, 0, sizeof m->fifo);
  pthread_mutex_init(&m->fifo.lock, NULL);
  int fd = mon_open_sock();
  if (fd < 0)
    return -1;
  m->fd = fd;
  m->addr4_changed = 1;
  m->addr6_changed = 1;
  return 0;
}

int rtnl_monitor_start(struct rtnl_mon_ctx *m)
{
  if (m->fd < 0)
    return -1;
  if (sys_pthread_create(&m->thread, NULL, rtnl_mon_thread, m) != 0)
    return -1;
  m->started = 1;
  return 0;
}

static void mon_destroy_ctx(struct rtnl_mon_ctx *m)
{
  if (m->fd > 0)
    sys_close(m->fd);
  m->fd = -1;
  pthread_mutex_destroy(&m->fifo.lock);
  free(m->fifo.items);
  memset(&m->fifo, 0, sizeof m->fifo);
}

void rtnl_monitor_stop(struct rtnl_mon_ctx *m)
{
  if (!m->started) {
    mon_destroy_ctx(m);
    return;
  }
  m->stop = 1;
  if (m->fd >= 0) {
    sys_close(m->fd);
    m->fd = -1;
  }
  pthread_join(m->thread, NULL);
  m->started = 0;
  mon_destroy_ctx(m);
}
