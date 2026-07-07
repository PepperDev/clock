#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <pthread.h>            // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <netdb.h>              // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"
#include "ioserv.h"

static int json_int(const char *p, const char *key)
{
  p = strstr(p, key);
  if (!p)
    return 0;
  p += strlen(key);
  p += strspn(p, " \t\r\n");
  if (*p != ':')
    return 0;
  p++;
  p += strspn(p, " \t\r\n\"");
  return atoi(p);
}

#define W_E_SUNNY   "\xe2\x98\x80\xef\xb8\x8f"
#define W_E_PCLOUDY "\xe2\x9b\x85"
#define W_E_CLOUDY  "\xe2\x98\x81\xef\xb8\x8f"
#define W_E_FOG     "\xf0\x9f\x8c\xab"
#define W_E_RAIN    "\xf0\x9f\x8c\xa7\xef\xb8\x8f"
#define W_E_SNOW    "\xf0\x9f\x8c\xa8"
#define W_E_THUNDER "\xe2\x9b\x88"

struct emoji_entry {
  int code;
  const char *emoji;
};
static const struct emoji_entry EMOJI_MAP[] = {
  {113, W_E_SUNNY},
  {116, W_E_PCLOUDY},
  {119, W_E_CLOUDY}, {122, W_E_CLOUDY},
  {143, W_E_FOG},
  {176, W_E_RAIN}, {263, W_E_RAIN}, {266, W_E_RAIN},
  {293, W_E_RAIN}, {296, W_E_RAIN}, {299, W_E_RAIN},
  {302, W_E_RAIN}, {305, W_E_RAIN}, {308, W_E_RAIN},
  {353, W_E_RAIN}, {356, W_E_RAIN},
  {179, W_E_SNOW}, {182, W_E_SNOW}, {185, W_E_SNOW},
  {227, W_E_SNOW}, {230, W_E_SNOW},
  {323, W_E_SNOW}, {326, W_E_SNOW}, {329, W_E_SNOW},
  {332, W_E_SNOW}, {335, W_E_SNOW}, {338, W_E_SNOW},
  {200, W_E_THUNDER}, {386, W_E_THUNDER}, {389, W_E_THUNDER},
  {392, W_E_THUNDER}, {395, W_E_THUNDER},
};

const char *weather_emoji(int code)
{
  for (size_t i = 0; i < sizeof EMOJI_MAP / sizeof EMOJI_MAP[0]; i++)
    if (EMOJI_MAP[i].code == code)
      return EMOJI_MAP[i].emoji;
  return "";
}

static void parse_desc(const char *body, char *desc, size_t sz)
{
  const char *vd = strstr(body, "\"weatherDesc\"");
  if (!vd)
    return;
  vd = strstr(vd, "\"value\"");
  if (!vd)
    return;
  vd += 7;
  vd += strspn(vd, " \t\r\n:");
  if (*vd != '"')
    return;
  vd++;
  const char *end = strchr(vd, '"');
  if (!end)
    return;
  size_t len = end - vd;
  if (len >= sz)
    len = sz - 1;
  memcpy(desc, vd, len);
  desc[len] = 0;
}

struct weather_csv_args {
  int temp, max, min, code;
  const char *desc;
};

static void weather_csv(char *out, size_t sz, const struct weather_csv_args *a)
{
  int n = snprintf(out, sz, "%d,%d,%d,%d,", a->temp, a->max, a->min, a->code);
  size_t remain = sz - (size_t)n;
  if (remain > 1) {
    strncpy(out + n, a->desc, remain - 1);
    out[sz - 1] = 0;
  }
}

static int weather_parse_from_body(const char *body, char *out, size_t outsz)
{
  struct weather_csv_args a;
  a.temp = json_int(body, "\"temp_C\"");
  a.code = json_int(body, "\"weatherCode\"");
  char desc[DESC_LEN] = { 0 };
  parse_desc(body, desc, sizeof desc);
  a.desc = desc;
  const char *w = strstr(body, "\"weather\":");
  a.max = 0;
  a.min = 0;
  if (w) {
    a.max = json_int(w, "\"maxtempC\"");
    a.min = json_int(w, "\"mintempC\"");
  }
  weather_csv(out, outsz, &a);
  return 0;
}

int weather_parse_resp(const char *resp, char *out, size_t outsz)
{
  const char *body = strstr(resp, "\r\n\r\n");
  if (!body)
    return -1;
  return weather_parse_from_body(body + 4, out, outsz);
}

void weather_dns_start(struct clock_state *ci)
{
  http_result_clear_cancelled(&ci->keep.async.weather_result);
  ci->keep.weather_state = WAN_DNS;
  dns_start(&ci->keep.async, &ci->keep.async.weather_dns, "wttr.in", AF_UNSPEC);
}

static int weather_ready(const struct cpu_keep *k, unsigned long long now)
{
  return stage_ready(&k->weather, now);
}

static void parse_csv(struct cpu_keep *k, const char *data)
{
  char tmp[WTHR_TEMP_SZ], max[WTHR_TEMP_SZ], min[WTHR_TEMP_SZ], code[WTHR_CODE_SZ], desc[DESC_LEN];
  /* widths match WTHR_TEMP_SZ-1, WTHR_CODE_SZ-1, DESC_LEN-1 */
  if (sscanf(data, "%4[^,],%4[^,],%4[^,],%5[^,],%63[^\n]", tmp, max, min, code, desc) >= 4) {
    k->weather_temp = atoi(tmp);
    k->weather_max = atoi(max);
    k->weather_min = atoi(min);
    k->weather_code = atoi(code);
    memcpy(k->weather_desc, desc, sizeof k->weather_desc - 1);
    k->weather_desc[sizeof k->weather_desc - 1] = 0;
    k->weather_valid = 1;
  }
}

static void pump_weather_ok(struct http_result *hr, struct cpu_keep *k)
{
  parse_csv(k, hr->data);
  k->weather_state = WAN_IDLE;
  fetch_ok_reset(&k->weather);
  hr->state = HTTP_IDLE;
  hr->cancelled = 0;
}

void try_refetch_weather(struct clock_state *ci, unsigned long long now)
{
  struct cpu_keep *k = &ci->keep;
  if (k->weather_refresh_sec <= 0)
    return;
  if (now - k->weather_refresh_ts < (unsigned long long)k->weather_refresh_sec)
    return;
  if (k->weather_state != WAN_IDLE)
    return;
  if (weather_ready(k, now) == 0)
    return;
  k->weather_refresh_ts = now;
  dns_cancel(&k->async.weather_dns);
  close_conn_fd(&k->weather_fd);
  http_result_cancel(&k->async.weather_result);
  weather_dns_start(ci);
}

static void pump_weather_done(struct clock_state *ci, unsigned long long now)
{
  struct cpu_keep *k = &ci->keep;
  struct http_result *r = &k->async.weather_result;
  r->state = HTTP_IDLE;
  k->weather_state = WAN_IDLE;
  fetch_err(&k->weather, now);
}

void pump_weather_result(struct clock_state *ci, unsigned long long now)
{
  struct cpu_keep *k = &ci->keep;
  struct http_result *wr = &k->async.weather_result;
  pthread_mutex_lock(&wr->lock);
  int s = wr->state;
  if (s == HTTP_DONE)
    pump_weather_ok(wr, k);
  else if (s == HTTP_ERROR)
    pump_weather_done(ci, now);
  pthread_mutex_unlock(&wr->lock);
}

void weather_cleanup(struct clock_state *ci)
{
  struct async_ctx *ctx = &ci->keep.async;
  io_cancel_all(&ctx->ioc, CANCEL_WEATHER);
  dns_cancel(&ctx->weather_dns);
  close_conn_fd(&ci->keep.weather_fd);
  http_result_cancel(&ctx->weather_result);
}

void restore_weather(struct clock_state *ci)
{
  if (!ci->keep.weather_valid)
    return;
  ci->weather_temp = ci->keep.weather_temp;
  ci->weather_max = ci->keep.weather_max;
  ci->weather_min = ci->keep.weather_min;
  ci->weather_code = ci->keep.weather_code;
  memcpy(ci->weather_desc, ci->keep.weather_desc, sizeof ci->weather_desc);
}
