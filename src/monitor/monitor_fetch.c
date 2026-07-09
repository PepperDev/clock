#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include <poll.h>               // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include "monitor_int.h"
#include "monitor.h"
#include "main.h"
#include "ioserv.h"
#include "util/syscall.h"

typedef int assert_retry_shift_safe[(RETRY_MAX <= 62) ? 1 : -1];

static void init_net_mutexes(struct async_ctx *ctx)
{
  pthread_mutex_init(&ctx->wan4_dns.lock, NULL);
  pthread_mutex_init(&ctx->wan6_dns.lock, NULL);
  pthread_mutex_init(&ctx->wan4_result.lock, NULL);
  pthread_mutex_init(&ctx->wan6_result.lock, NULL);
}

static void init_weather_mutexes(struct async_ctx *ctx)
{
  pthread_mutex_init(&ctx->weather_dns.lock, NULL);
  pthread_mutex_init(&ctx->weather_result.lock, NULL);
}

void async_ctx_init(struct async_ctx *ctx, int *wan4_fd, int *wan6_fd, int *weather_fd, unsigned int widget_mask)
{
  if (widget_mask & (1u << WIDGET_NET))
    init_net_mutexes(ctx);
  if (widget_mask & (1u << WIDGET_WEATHER))
    init_weather_mutexes(ctx);
  ctx->widget_mask = widget_mask;
  if (widget_mask & ((1u << WIDGET_NET) | (1u << WIDGET_WEATHER)))
    io_init(&ctx->ioc);
  ctx->ioc.owner = ctx;
  ctx->wan4_fd = wan4_fd;
  ctx->wan6_fd = wan6_fd;
  ctx->weather_fd = weather_fd;
}

void close_conn_fd(int *fd)
{
  int old = __atomic_exchange_n(fd, -1, __ATOMIC_ACQ_REL);
  if (old >= 0)
    sys_close(old);
}

static void pump_wan_ok(struct http_result *hr, struct cpu_keep *k, int v6)
{
  if (v6) {
    strncpy(k->pub_ip6, hr->data, sizeof k->pub_ip6 - 1);
    k->pub_ip6[sizeof k->pub_ip6 - 1] = 0;
    fetch_ok_reset(&k->wan6);
    k->wan6_state = WAN_IDLE;
  } else {
    strncpy(k->pub_ip4, hr->data, sizeof k->pub_ip4 - 1);
    k->pub_ip4[sizeof k->pub_ip4 - 1] = 0;
    fetch_ok_reset(&k->wan4);
    k->wan4_state = WAN_IDLE;
  }
  hr->state = HTTP_IDLE;
  hr->cancelled = 0;
}

void fetch_ok_reset(struct fetch_retry *fr)
{
  fr->try = 0;
}

void fetch_err(struct fetch_retry *fr, unsigned long long now)
{
  if (fr->try >= RETRY_MAX)
    return;
  unsigned long long b = (unsigned long long)4 << fr->try;
  fr->try++;
  fr->retry_ts = now + b > now ? now + b : (unsigned long long)-1;
}

static void pump_wan_err(struct http_result *hr, struct cpu_keep *k, int v6, unsigned long long now)
{
  hr->state = HTTP_IDLE;
  hr->cancelled = 0;
  struct fetch_retry *fr = v6 ? &k->wan6 : &k->wan4;
  if (v6)
    k->wan6_state = WAN_IDLE;
  else
    k->wan4_state = WAN_IDLE;
  fetch_err(fr, now);
}

static void pump_wan_result(struct clock_state *ci, int v6, unsigned long long now)
{
  struct cpu_keep *k = &ci->keep;
  struct http_result *hr = v6 ? &k->async.wan6_result : &k->async.wan4_result;
  pthread_mutex_lock(&hr->lock);
  int s = hr->state;
  if (s == HTTP_DONE) {
    pump_wan_ok(hr, k, v6);
  } else if (s == HTTP_ERROR) {
    pump_wan_err(hr, k, v6, now);
  }
  pthread_mutex_unlock(&hr->lock);
}

// cppcheck-suppress staticFunction
int any_ready(unsigned char t, unsigned long long retry, unsigned long long now)
{
  if (t == 0)
    return 1;
  if (t >= RETRY_MAX)
    return 0;
  if (now >= retry)
    return 1;
  return 0;
}

int stage_ready(const struct fetch_retry *fr, unsigned long long now)
{
  return any_ready(fr->try, fr->retry_ts, now);
}

static int wan_ready(const struct cpu_keep *k, unsigned long long now, int v6)
{
  return stage_ready(v6 ? &k->wan6 : &k->wan4, now);
}

static void wan_restart_fetch(struct clock_state *ci, int v6)
{
  struct http_result *hr = v6 ? &ci->keep.async.wan6_result : &ci->keep.async.wan4_result;
  pthread_mutex_lock(&hr->lock);
  hr->cancelled = 1;
  pthread_mutex_unlock(&hr->lock);
  int *fd = v6 ? &ci->keep.wan6_fd : &ci->keep.wan4_fd;
  int old_fd = __atomic_exchange_n(fd, -1, __ATOMIC_ACQ_REL);
  if (old_fd >= 0)
    shutdown(old_fd, SHUT_RDWR);
  dns_cancel(wan_dns_slot(&ci->keep.async, v6));
  wan_dns_start(ci, v6);
}

static void refetch_wan_target(struct clock_state *ci, int v6)
{
  struct http_result *hr = v6 ? &ci->keep.async.wan6_result : &ci->keep.async.wan4_result;
  pthread_mutex_lock(&hr->lock);
  int done = hr->state >= HTTP_DONE;
  pthread_mutex_unlock(&hr->lock);
  if (done)
    return;
  wan_restart_fetch(ci, v6);
}

static unsigned long long *wan_rts(struct cpu_keep *k, int v6)
{
  return v6 ? &k->v6_ip_refresh_ts : &k->v4_ip_refresh_ts;
}

static void try_wan(struct clock_state *ci, unsigned long long now, int v6)
{
  struct cpu_keep *k = &ci->keep;
  unsigned long long *rts = wan_rts(k, v6);
  if (!wan_ready(k, now, v6))
    return;
  const struct fetch_retry *fr = v6 ? &k->wan6 : &k->wan4;
  if (!fr->try) {
    if (now - *rts < (unsigned long long)k->ip_refresh_sec)
      return;
    *rts = now;
  }
  refetch_wan_target(ci, v6);
}

static void try_refetch_wan(struct clock_state *ci, unsigned long long now)
{
  if (ci->keep.ip_refresh_sec <= 0)
    return;
  try_wan(ci, now, 0);
  try_wan(ci, now, 1);
}

void poll_async_fetches(struct clock_state *ci, time_t now)
{
  if (widget_active(&ci->keep.widget, WIDGET_NET)) {
    try_refetch_wan(ci, now);
    pump_wan_result(ci, 0, now);
    pump_wan_result(ci, 1, now);
  }
  if (widget_active(&ci->keep.widget, WIDGET_WEATHER)) {
    try_refetch_weather(ci, now);
    pump_weather_result(ci, now);
  }
}

static void once_net_cleanup(struct clock_state *ci)
{
  struct async_ctx *ctx = &ci->keep.async;
  io_cancel_all(&ctx->ioc, CANCEL_WAN);
  dns_cancel(wan_dns_slot(ctx, 0));
  dns_cancel(wan_dns_slot(ctx, 1));
  http_result_cancel(&ctx->wan4_result);
  http_result_cancel(&ctx->wan6_result);
}

static int wan_result_terminal(const struct http_result *hr)
{
  return hr->state == HTTP_DONE || hr->state == HTTP_ERROR;
}

static int wan_result_ready(struct http_result *hr)
{
  pthread_mutex_lock(&hr->lock);
  int r = wan_result_terminal(hr);
  pthread_mutex_unlock(&hr->lock);
  return r;
}

static void wan_try_read(struct http_result *hr)
{
  if (pthread_mutex_trylock(&hr->lock) == 0)
    pthread_mutex_unlock(&hr->lock);
}

static int check_one_wan(struct http_result *hr, struct http_result *other)
{
  if (!wan_result_ready(hr))
    return 0;
  wan_try_read(other);
  return 1;
}

static int both_wan_ready(struct clock_state *ci)
{
  struct async_ctx *ctx = &ci->keep.async;
  if (ci->keep.ipv4_local[0] && ci->keep.ipv6_local[0])
    return wan_result_ready(&ctx->wan4_result) && wan_result_ready(&ctx->wan6_result);
  if (ci->keep.ipv4_local[0])
    return check_one_wan(&ctx->wan4_result, &ctx->wan6_result);
  if (ci->keep.ipv6_local[0])
    return check_one_wan(&ctx->wan6_result, &ctx->wan4_result);
  return 1;
}

static int active_wan_ready(struct clock_state *ci)
{
  return !widget_active(&ci->keep.widget, WIDGET_NET) || both_wan_ready(ci);
}

static int active_weather_ready(struct clock_state *ci)
{
  if (!widget_active(&ci->keep.widget, WIDGET_WEATHER))
    return 1;
  struct async_ctx *ctx = &ci->keep.async;
  pthread_mutex_lock(&ctx->weather_result.lock);
  int r = ctx->weather_result.state >= HTTP_DONE;
  pthread_mutex_unlock(&ctx->weather_result.lock);
  return r;
}

static void once_cleanup_widgets(struct clock_state *ci)
{
  if (widget_active(&ci->keep.widget, WIDGET_NET))
    once_net_cleanup(ci);
  if (widget_active(&ci->keep.widget, WIDGET_WEATHER))
    weather_cleanup(ci);
}

static int async_done(struct clock_state *ci)
{
  return active_weather_ready(ci) && active_wan_ready(ci);
}

static void once_pump_results(struct clock_state *ci, unsigned long long now)
{
  if (widget_active(&ci->keep.widget, WIDGET_WEATHER))
    pump_weather_result(ci, now);
  if (widget_active(&ci->keep.widget, WIDGET_NET)) {
    pump_wan_result(ci, 0, now);
    pump_wan_result(ci, 1, now);
  }
}

void once_wait(struct clock_state *ci, unsigned long long t0)
{
  if (!widget_active(&ci->keep.widget, WIDGET_NET) && !widget_active(&ci->keep.widget, WIDGET_WEATHER))
    return;
  unsigned long long dl = t0 + ONCE_WAIT_SEC;
  unsigned long long now = t0;
  while (!async_done(ci) && now < dl) {
    if (tls_terminated)
      break;
    poll(NULL, 0, POLL_WAIT_MS);
    now = (unsigned long long)time(NULL);
  }
  once_pump_results(ci, now);
  once_cleanup_widgets(ci);
}
