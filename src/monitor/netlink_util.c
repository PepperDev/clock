#define _GNU_SOURCE
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <linux/netlink.h>      // cppcheck-suppress missingIncludeSystem
#include <errno.h>              // cppcheck-suppress missingIncludeSystem
#include <poll.h>               // cppcheck-suppress missingIncludeSystem
#include <linux/genetlink.h>    // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "main.h"

enum { NL_RECV_BUDGET = 64, NL_DRAIN_MAX = 4 };

static ssize_t nlk_send(int fd, const struct msghdr *msg)
{
  ssize_t r;
  while ((r = sys_sendmsg(fd, msg, 0)) < 0 && errno == EINTR) ;
  return r;
}

ssize_t nlk_recv(int fd, struct msghdr *msg, int flags)
{
  if (!(flags & MSG_DONTWAIT)) {
    struct pollfd pfd = {.fd = fd,.events = POLLIN };
    if (sys_poll(&pfd, 1, POLL_WAIT_MS) <= 0)
      return -1;
    if (tls_terminated) {
      errno = EINTR;
      return -1;
    }
  }
  ssize_t r;
  while ((r = sys_recvmsg(fd, msg, flags)) < 0 && errno == EINTR) ;
  return r;
}

int talk(const struct netlink_ctx *nlk, void *buf)
{
  struct sockaddr_nl sa = {.nl_family = AF_NETLINK };
  struct nlmsghdr *nh = (struct nlmsghdr *)buf;
  struct iovec iov = {.iov_base = buf,.iov_len = nh->nlmsg_len };
  struct msghdr msg = {.msg_name = &sa,.msg_namelen = sizeof sa,.msg_iov = &iov,.msg_iovlen = 1 };
  if (nlk_send(nlk->fd, &msg) < 0)
    return -1;
  iov.iov_len = NLBUF;
  ssize_t r = nlk_recv(nlk->fd, &msg, 0);
  if (r < 0)
    return -1;
  nh->nlmsg_len = (unsigned)r;
  return 0;
}

static int recv_done_loop(const struct netlink_ctx *nlk)
{
  int budget = NL_RECV_BUDGET;
  unsigned char done[NLBUF];
  struct iovec di = {.iov_base = done,.iov_len = sizeof done };
  struct msghdr dm = {.msg_iov = &di,.msg_iovlen = 1 };
  for (;;) {
    if (--budget == 0)
      return -1;
    ssize_t r = nlk_recv(nlk->fd, &dm, 0);
    if (r < 0)
      return -1;
    struct nlmsghdr *dh = (struct nlmsghdr *)done;
    dh->nlmsg_len = (unsigned)r;
    if (!NLMSG_OK(dh, dh->nlmsg_len))
      return -1;
    if (dh->nlmsg_type == NLMSG_DONE)
      return 0;
  }
}

int talk_dump(const struct netlink_ctx *nlk, void *buf)
{
  if (talk(nlk, buf) < 0)
    return -1;
  const struct nlmsghdr *nh = (const struct nlmsghdr *)buf;
  if (!(nh->nlmsg_flags & NLM_F_MULTI))
    return 0;
  return recv_done_loop(nlk);
}

void init_req(struct nlmsghdr *nh, struct genlmsghdr *gh, struct netlink_ctx *nlk, int type, int cmd)
{
  nh->nlmsg_type = (unsigned short)type;
  nh->nlmsg_flags = NLM_F_REQUEST;
  nh->nlmsg_seq = ++nlk->seq;
  gh->cmd = (unsigned char)cmd;
}

static int nl_handle_cb(const struct nlmsghdr *h, nl_dump_cb cb, void *arg)
{
  if (!cb)
    return 0;
  int r = cb(h, arg);
  if (r < 0)
    return -1;
  return r > 0 ? 2 : 0;
}

int nl_handle_buf(unsigned char *buf, size_t len, nl_dump_cb cb, void *arg)
{
  size_t rem = len;
  struct nlmsghdr *h;
  for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, rem); h = NLMSG_NEXT(h, rem)) {
    if (h->nlmsg_type == NLMSG_DONE)
      return 0;
    if (h->nlmsg_type == NLMSG_ERROR)
      return -1;
    int cr = nl_handle_cb(h, cb, arg);
    if (cr)
      return cr;
  }
  return 1;
}

int nl_drain_done(int fd)
{
  unsigned char buf[NLBUF];
  struct iovec iov = {.iov_base = buf,.iov_len = sizeof buf };
  struct msghdr msg = {.msg_iov = &iov,.msg_iovlen = 1 };
  for (int i = 0; i < NL_DRAIN_MAX; i++) {
    if (nlk_recv(fd, &msg, MSG_DONTWAIT) < 0)
      break;
    if (((struct nlmsghdr *)buf)->nlmsg_type == NLMSG_DONE)
      break;
  }
  return 0;
}

static int nl_recv_loop(const struct netlink_ctx *nlk, nl_dump_cb cb, void *arg)
{
  int budget = NL_RECV_BUDGET;
  unsigned char buf[NLBUF];
  for (;;) {
    if (--budget == 0)
      return -1;
    struct iovec ri = {.iov_base = buf,.iov_len = sizeof buf };
    struct msghdr rm = {.msg_iov = &ri,.msg_iovlen = 1 };
    ssize_t n = nlk_recv(nlk->fd, &rm, 0);
    if (n < 0)
      return -1;
    int st = nl_handle_buf(buf, (size_t)n, cb, arg);
    if (st == 2) {
      nl_drain_done(nlk->fd);
      return 0;
    }
    if (st <= 0)
      return st;
  }
}

int nl_send_dump(int fd, unsigned char *buf, size_t len)
{
  struct sockaddr_nl sa = {.nl_family = AF_NETLINK };
  struct iovec iov = {.iov_base = buf,.iov_len = len };
  struct msghdr msg = {.msg_name = &sa,.msg_namelen = sizeof sa,.msg_iov = &iov,.msg_iovlen = 1 };
  ssize_t r = nlk_send(fd, &msg);
  return r < 0 ? -1 : (int)r;
}

int nl_dump_iter(struct netlink_ctx *nlk, int cmd, nl_dump_cb cb, void *arg)
{
  unsigned char buf[NLBUF] = { 0 };
  struct nlmsghdr *nh = (struct nlmsghdr *)buf;
  nh->nlmsg_len = sizeof(struct nlmsghdr) + GENL_HDRLEN;
  init_req(nh, NLMSG_DATA(nh), nlk, nlk->family, cmd);
  nh->nlmsg_flags |= NLM_F_DUMP;
  if (nl_send_dump(nlk->fd, buf, nh->nlmsg_len) < 0)
    return -1;
  return nl_recv_loop(nlk, cb, arg);
}

int nlk_init(struct netlink_ctx *nlk)
{
  nlk->family = -1;
  nlk->seq = 0;
  nlk->fd = sys_socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
  if (nlk->fd < 0)
    return -1;
  struct sockaddr_nl sa = {.nl_family = AF_NETLINK };
  if (sys_bind(nlk->fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
    sys_close(nlk->fd);
    nlk->fd = -1;
    return -1;
  }
  return 0;
}
