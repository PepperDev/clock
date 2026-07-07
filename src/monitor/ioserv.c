#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
enum { INIT_RESP_CAP = 4096 };
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <errno.h>              // cppcheck-suppress missingIncludeSystem
#include <limits.h>             // cppcheck-suppress missingIncludeSystem
#include <poll.h>               // cppcheck-suppress missingIncludeSystem
#include <sys/eventfd.h>        // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include "monitor_int.h"
#include "monitor.h"
#include "ioserv.h"
#include "util/syscall.h"

void io_close_conn(struct io_conn *c)
{
  io_cancel_conn(c);
  free(c->resp);
  c->resp = NULL;
  c->resp_cap = 0;
  c->phase = 0;
  c->req_sent = 0;
  c->resp_len = 0;
}

static void io_close_matching(struct io_conn *conns, int n, int kind)
{
  for (int i = 0; i < n; i++)
    if (!kind || (kind == 1) == conns[i].is_wan)
      io_close_conn(&conns[i]);
}

static int io_cmd(struct ioserv_ctl *ctl, struct io_conn *conns, int n)
{
  pthread_mutex_lock(&ctl->lock);
  if (ctl->shutdown) {
    pthread_mutex_unlock(&ctl->lock);
    return -1;
  }
  if (ctl->cancel_all) {
    io_close_matching(conns, n, CANCEL_ALL);
    ctl->cancel_all = 0;
  }
  if (ctl->wan_cancel) {
    io_close_matching(conns, n, CANCEL_WAN);
    ctl->wan_cancel = 0;
  }
  if (ctl->weather_cancel) {
    io_close_matching(conns, n, CANCEL_WEATHER);
    ctl->weather_cancel = 0;
  }
  ctl->pending_work = 0;
  pthread_mutex_unlock(&ctl->lock);
  return 0;
}

static int io_create_conn(struct io_conn *c, const struct sockaddr_storage *ss, int addrlen)
{
  c->fd = socket(ss->ss_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (c->fd < 0) {
    http_result_write(c->result, NULL, 0);
    return -1;
  }
  __atomic_store_n(c->target_fd, c->fd, __ATOMIC_RELEASE);
  if (connect(c->fd, (const struct sockaddr *)ss, addrlen) < 0 && errno != EINPROGRESS && errno != EINTR) {
    sys_close(c->fd);
    c->fd = -1;
    __atomic_store_n(c->target_fd, -1, __ATOMIC_RELEASE);
    http_result_write(c->result, NULL, 'C');
    return -1;
  }
  return 0;
}

static int io_start_conn(struct io_conn *c)
{
  struct sockaddr_storage ss;
  int r = dns_read_slot(c->dns, &ss, sizeof ss);
  if (r < 0) {
    http_result_write(c->result, NULL, 'D');
    return -1;
  }
  if (r == 0)
    return 0;
  if (io_create_conn(c, &ss, r) < 0)
    return -1;
  c->phase = 1;
  return 1;
}

static void io_check_dns(struct io_conn *conns, int n)
{
  for (int i = 0; i < n; i++)
    if (conns[i].phase == 0)
      io_start_conn(&conns[i]);
}

static int has_dns_running(const struct io_conn *conns, int n)
{
  for (int i = 0; i < n; i++) {
    struct dns_slot *s = conns[i].dns;
    if (s) {
      pthread_mutex_lock(&s->lock);
      int running = (s->state == DNS_RUNNING);
      pthread_mutex_unlock(&s->lock);
      if (running)
        return 1;
    }
  }
  return 0;
}

static int io_idle(struct ioserv_ctl *ctl, const struct io_conn *conns, int n)
{
  for (int i = 0; i < n; i++)
    if (conns[i].phase)
      return 0;
  pthread_mutex_lock(&ctl->lock);
  if (ctl->pending_work | ctl->cancel_all | ctl->wan_cancel | ctl->weather_cancel || has_dns_running(conns, n)) {
    pthread_mutex_unlock(&ctl->lock);
    return 0;
  }
  ctl->io_thread_active = 0;
  int efd = ctl->efd;
  ctl->efd = -1;
  pthread_mutex_unlock(&ctl->lock);
  if (efd >= 0)
    sys_close(efd);
  return 1;
}

static void io_conn_error(struct io_conn *c)
{
  http_result_write(c->result, NULL, c->phase == 1 ? 'C' : 'H');
  io_close_conn(c);
}

static void io_conn_ready(struct io_conn *c)
{
  int err = 0;
  socklen_t elen = sizeof err;
  getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &elen);
  if (err) {
    io_conn_error(c);
    return;
  }
  c->phase = 2;
  c->req_sent = 0;
}

static int io_send(int fd, const void *buf, size_t len)
{
  int n;
  while ((n = (int)sys_send(fd, buf, len, MSG_NOSIGNAL)) < 0 && errno == EINTR) ;
  return n;
}

static void io_conn_send(struct io_conn *c)
{
  int n = io_send(c->fd, c->req + c->req_sent, c->req_len - c->req_sent);
  if (n <= 0) {
    if (n == 0 || errno != EAGAIN)
      io_conn_error(c);
    return;
  }
  c->req_sent += n;
  if (c->req_sent >= c->req_len)
    c->phase = 3;
}

static void io_conn_done(struct io_conn *c)
{
  const char *body = "";
  if (c->resp) {
    c->resp[c->resp_len] = 0;
    body = c->resp;
  }
  char data[HTTP_RESULT_DATA_SZ] = { 0 };
  if (c->parse && c->parse(body, data, sizeof data) == 0)
    http_result_write(c->result, data, 0);
  else
    http_result_write(c->result, NULL, 'H');
  io_close_conn(c);
}

static int grow_resp(struct io_conn *c)
{
  if (c->resp_cap > INT_MAX / 2)
    return -1;
  unsigned cap = c->resp_cap ? (unsigned)c->resp_cap * 2 : INIT_RESP_CAP;
  cap = (cap + 63) & ~63U;
  char *p = realloc(c->resp, cap);
  if (!p)
    return -1;
  c->resp = p;
  c->resp_cap = (int)cap;
  return 0;
}

static int io_recv(int fd, void *buf, size_t len)
{
  int n;
  while ((n = sys_recv(fd, buf, len, 0)) < 0 && errno == EINTR) ;
  return n;
}

static void io_conn_recv(struct io_conn *c)
{
  if (c->resp_len + 1 >= c->resp_cap && grow_resp(c) < 0) {
    io_conn_error(c);
    return;
  }
  int n = io_recv(c->fd, c->resp + c->resp_len, c->resp_cap - 1 - c->resp_len);
  if (n > 0) {
    c->resp_len += n;
    return;
  }
  if (n == 0) {
    io_conn_done(c);
    return;
  }
  if (errno != EAGAIN)
    io_conn_error(c);
}

static void io_conn_event(struct io_conn *c, int revents)
{
  if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
    io_conn_error(c);
    return;
  }
  if (c->phase == 1)
    io_conn_ready(c);
  else if (c->phase == 2)
    io_conn_send(c);
  else if (c->phase == 3)
    io_conn_recv(c);
}

static int io_build_pfds(struct pollfd pf[], struct io_conn *pm[], struct io_conn co[], int n, int ef)
{
  int f = 0;
  if (ef >= 0) {
    pf[f].fd = ef;
    pf[f].events = POLLIN;
    pm[f] = NULL;
    f++;
  }
  for (int i = 0; i < n; i++) {
    if (co[i].fd < 0)
      continue;
    pf[f].fd = co[i].fd;
    pf[f].events = co[i].phase == 3 ? POLLIN : POLLOUT;
    pm[f] = &co[i];
    f++;
  }
  return f;
}

static void io_poll_events(const struct pollfd *pf, struct io_conn **pmap, int nfds)
{
  int start = 0;
  if (!pmap[0] && (pf[0].revents & POLLIN)) {
    uint64_t v;
    sys_read(pf[0].fd, &v, sizeof v);
    start = 1;
  }
  for (int i = start; i < nfds; i++)
    if (pmap[i] && (pf[i].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL)))
      io_conn_event(pmap[i], pf[i].revents);
}

/* returns: 0 = continue, 1 = idle exit, -1 = shutdown/error */
int io_loop_once(struct ioserv_ctl *ctl, struct io_conn *conns, int n, struct pollfd *pf, struct io_conn **pmap)
{
  if (io_cmd(ctl, conns, n) < 0)
    return -1;
  io_check_dns(conns, n);
  if (io_idle(ctl, conns, n))
    return 1;
  int nfds = io_build_pfds(pf, pmap, conns, n, ctl->efd);
  if (nfds <= 0)
    return -1;
  int rc = poll(pf, nfds, 100);
  if (rc < 0) {
    if (errno == EINTR)
      return 0;
    return -1;
  }
  io_poll_events(pf, pmap, nfds);
  return 0;
}
