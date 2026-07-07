#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <errno.h>              // cppcheck-suppress missingIncludeSystem
#include <poll.h>               // cppcheck-suppress missingIncludeSystem
#include <sys/eventfd.h>        // cppcheck-suppress missingIncludeSystem
#include "monitor_int.h"
#include "monitor.h"
#include "ioserv.h"
#include "util/syscall.h"

#define MAX_POLL_FDS 8

static void io_setup_one(struct io_conn *c, struct dns_slot *d, struct http_result *r, const char *req,
                         int (*parse)(const char *, char *, size_t))
{
  c->dns = d;
  c->result = r;
  c->req_len = snprintf(c->req, sizeof c->req, "%s", req);
  c->parse = parse;
}

static void io_setup_weather(struct io_conn *c, struct async_ctx *ctx)
{
  io_setup_one(c, &ctx->weather_dns, &ctx->weather_result,
               "GET /?format=j1 HTTP/1.0\r\nHost: wttr.in\r\nConnection: close\r\n\r\n", weather_parse_resp);
  c->is_wan = 0;
  c->target_fd = ctx->weather_fd;
}

static int io_setup_wan_conns(struct io_conn *conns, struct async_ctx *ctx, int n)
{
  io_setup_one(&conns[n], wan_dns_slot(ctx, 0), &ctx->wan4_result,
               "GET / HTTP/1.0\r\nHost: api.ipify.org\r\nConnection: close\r\n\r\n", parse_body);
  conns[n].is_wan = 1;
  conns[n].target_fd = ctx->wan4_fd;
  n++;
  io_setup_one(&conns[n], wan_dns_slot(ctx, 1), &ctx->wan6_result,
               "GET / HTTP/1.0\r\nHost: api6.ipify.org\r\nConnection: close\r\n\r\n", parse_body);
  conns[n].is_wan = 1;
  conns[n].target_fd = ctx->wan6_fd;
  n++;
  return n;
}

/* used by test suite cross-file, cannot be static */
int io_setup_conns(struct async_ctx *ctx, struct io_conn *conns)        // cppcheck-suppress staticFunction
{
  if (!ctx)
    return 0;
  int n = 0;
  if (ctx->widget_mask & (1u << WIDGET_NET))
    n = io_setup_wan_conns(conns, ctx, n);
  if (ctx->widget_mask & (1u << WIDGET_WEATHER)) {
    io_setup_weather(&conns[n], ctx);
    n++;
  }
  return n;
}

static void io_thread_exit(struct ioserv_ctl *ctl, int nconns)
{
  pthread_mutex_lock(&ctl->lock);
  ctl->io_thread_active = 0;
  int efd = ctl->efd;
  ctl->efd = -1;
  pthread_mutex_unlock(&ctl->lock);
  if (efd >= 0)
    sys_close(efd);
  for (int i = 0; i < nconns; i++)
    io_close_conn(&ctl->conns[i]);
}

static int io_thread_init(struct ioserv_ctl *ctl)
{
  pthread_mutex_lock(&ctl->lock);
  if (ctl->efd < 0) {
    int efd = eventfd(0, EFD_NONBLOCK);
    if (efd < 0) {
      ctl->io_thread_active = 0;
      pthread_mutex_unlock(&ctl->lock);
      return -1;
    }
    ctl->efd = efd;
  }
  ctl->io_thread_active = 1;
  pthread_mutex_unlock(&ctl->lock);
  return 0;
}

static int io_init_conns(struct ioserv_ctl *ctl)
{
  if (io_thread_init(ctl) < 0)
    return -1;
  pthread_mutex_lock(&ctl->lock);
  memset(ctl->conns, 0, sizeof(struct io_conn) * MAX_HTTP_CONNS);
  for (int i = 0; i < MAX_HTTP_CONNS; i++)
    ctl->conns[i].fd = -1;
  int shutting_down = ctl->shutdown;
  pthread_mutex_unlock(&ctl->lock);
  if (shutting_down)
    return -1;
  return io_setup_conns(ctl->owner, ctl->conns);
}

void *io_thread_run(void *arg)
{
  struct ioserv_ctl *ctl = arg;
  int nconns = io_init_conns(ctl);
  if (nconns <= 0) {
    io_thread_exit(ctl, 0);
    return NULL;
  }
  struct pollfd pf[MAX_POLL_FDS];
  struct io_conn *pmap[MAX_POLL_FDS];
  while (1) {
    int r = io_loop_once(ctl, ctl->conns, nconns, pf, pmap);
    if (r < 0)
      break;
    if (r > 0)
      return NULL;
  }
  io_thread_exit(ctl, nconns);
  return NULL;
}
