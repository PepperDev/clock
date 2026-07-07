#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <stdint.h>             // cppcheck-suppress missingIncludeSystem
#include <pthread.h>            // cppcheck-suppress missingIncludeSystem
#include <sys/eventfd.h>        // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <netdb.h>              // cppcheck-suppress missingIncludeSystem
#include "monitor_int.h"
#include "monitor.h"
#include "main.h"
#include "ioserv.h"
#include "util/syscall.h"

static void dns_cleanup(void *arg)
{
  struct dns_arg *d = arg;
  if (d->ai)
    freeaddrinfo(d->ai);
  free(d);
}

static void io_spawn(struct ioserv_ctl *ioc, int efd)
{
  pthread_t th;
  if (sys_pthread_create(&th, NULL, io_thread_run, ioc) != 0) {
    pthread_mutex_lock(&ioc->lock);
    ioc->efd = -1;
    ioc->io_thread_active = 0;
    ioc->pending_work--;
    pthread_mutex_unlock(&ioc->lock);
    sys_close(efd);
    return;
  }
  pthread_detach(th);
}

static void ensure_io_thread(struct ioserv_ctl *ioc)
{
  pthread_mutex_lock(&ioc->lock);
  if (ioc->shutdown) {
    pthread_mutex_unlock(&ioc->lock);
    return;
  }
  if (!ioc->io_thread_active) {
    int efd = eventfd(0, EFD_NONBLOCK);
    if (efd < 0) {
      pthread_mutex_unlock(&ioc->lock);
      return;
    }
    ioc->efd = efd;
    ioc->io_thread_active = 1;
    ioc->pending_work++;
    pthread_mutex_unlock(&ioc->lock);
    io_spawn(ioc, efd);
    return;
  }
  ioc->pending_work++;
  int efd = ioc->efd;
  pthread_mutex_unlock(&ioc->lock);
  uint64_t v = 1;
  sys_write(efd, &v, sizeof v);
}

static void store_result(struct dns_slot *slot, const struct sockaddr_storage *ss, socklen_t slen)
{
  pthread_mutex_lock(&slot->lock);
  if (slot->cancelled) {
    slot->state = DNS_ERROR;
  } else if (slen > 0) {
    slot->addrlen = slen;
    memcpy(&slot->addr, ss, slen);
    slot->state = DNS_DONE;
  } else {
    slot->state = DNS_ERROR;
  }
  pthread_mutex_unlock(&slot->lock);
}

static void exit_thread(int oldtype)
{
  pthread_setcanceltype(oldtype, NULL);
  pthread_detach(pthread_self());
}

static void *dns_thread_run(void *arg)
{
  struct dns_arg *d = arg;
  int oldtype;
  pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);
  pthread_cleanup_push(dns_cleanup, d);
  struct addrinfo hints = {.ai_family = d->family,.ai_socktype = SOCK_STREAM };
  struct addrinfo *ai = NULL;
  struct sockaddr_storage ss;
  socklen_t slen = 0;
  if (getaddrinfo(d->host, "80", &hints, &ai) == 0 && ai) {
    slen = ai->ai_addrlen;
    memcpy(&ss, ai->ai_addr, slen);
    freeaddrinfo(ai);
  }
  store_result(d->slot, &ss, slen);
  ensure_io_thread(&d->ctx->ioc);
  pthread_cleanup_pop(1);
  exit_thread(oldtype);
  return NULL;
}

int dns_read_slot(struct dns_slot *slot, struct sockaddr_storage *ss, socklen_t salen)
{
  pthread_mutex_lock(&slot->lock);
  int s = slot->state;
  if (s == DNS_DONE) {
    socklen_t n = slot->addrlen < salen ? slot->addrlen : salen;
    memcpy(ss, &slot->addr, n);
    socklen_t ret = slot->addrlen;
    slot->state = DNS_IDLE;
    slot->cancelled = 0;
    pthread_mutex_unlock(&slot->lock);
    return ret;
  }
  if (s == DNS_ERROR) {
    slot->state = DNS_IDLE;
    slot->cancelled = 0;
    pthread_mutex_unlock(&slot->lock);
    return -1;
  }
  pthread_mutex_unlock(&slot->lock);
  return 0;
}

struct dns_slot *wan_dns_slot(struct async_ctx *ctx, int v6)
{
  return v6 ? &ctx->wan6_dns : &ctx->wan4_dns;
}

void dns_cancel(struct dns_slot *slot)
{
  pthread_mutex_lock(&slot->lock);
  if (slot->state != DNS_RUNNING) {
    pthread_mutex_unlock(&slot->lock);
    return;
  }
  slot->cancelled = 1;
  pthread_t t = slot->thread;
  slot->state = DNS_IDLE;
  pthread_mutex_unlock(&slot->lock);
  pthread_cancel(t);
  pthread_detach(t);
}

static struct dns_arg *dns_arg_new(struct async_ctx *ctx, struct dns_slot *slot, const char *host, int family)
{
  struct dns_arg *arg = malloc(sizeof *arg);
  if (!arg)
    return NULL;
  arg->slot = slot;
  arg->host = host;
  arg->family = family;
  arg->ai = NULL;
  arg->ctx = ctx;
  return arg;
}

static int start_dns(struct async_ctx *ctx, struct dns_slot *slot, const char *host, int family)
{
  struct dns_arg *arg = dns_arg_new(ctx, slot, host, family);
  if (!arg)
    return 0;
  pthread_t th;
  if (sys_pthread_create(&th, NULL, dns_thread_run, arg)) {
    free(arg);
    return 0;
  }
  pthread_mutex_lock(&slot->lock);
  slot->state = DNS_RUNNING;
  slot->cancelled = 0;
  slot->thread = th;
  pthread_mutex_unlock(&slot->lock);
  return 1;
}

void dns_start(struct async_ctx *ctx, struct dns_slot *slot, const char *host, int family)
{
  start_dns(ctx, slot, host, family);
}

void wan_dns_start(struct clock_state *ci, int v6)
{
  struct async_ctx *ctx = &ci->keep.async;
  enum wan_state *st;
  int *fd;
  if (v6) {
    st = &ci->keep.wan6_state;
    fd = &ci->keep.wan6_fd;
  } else {
    st = &ci->keep.wan4_state;
    fd = &ci->keep.wan4_fd;
  }
  http_result_clear_cancelled(v6 ? &ctx->wan6_result : &ctx->wan4_result);
  *st = WAN_IDLE;
  *fd = -1;
  int ok = v6 ? start_dns(ctx, &ctx->wan6_dns, "api6.ipify.org", AF_INET6)
      : start_dns(ctx, &ctx->wan4_dns, "api.ipify.org", AF_INET);
  if (ok)
    *st = WAN_DNS;
}
