#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <strings.h>            // cppcheck-suppress missingIncludeSystem
#include "widget.h"

static const char *NAMES[WIDGET_COUNT] = {
  [WIDGET_DATE] = "DATE",
  [WIDGET_CPU] = "CPU",
  [WIDGET_GPU] = "GPU",
  [WIDGET_MEM] = "MEM",
  [WIDGET_FAN] = "FAN",
  [WIDGET_BAT] = "BAT",
  [WIDGET_UP] = "UP",
  [WIDGET_STO] = "STO",
  [WIDGET_NET] = "NET",
  [WIDGET_WEATHER] = "WEATHER",
  [WIDGET_CAL] = "CAL",
};

static const WidgetType DEFAULT_ORDER[] = {
  WIDGET_DATE, WIDGET_CPU, WIDGET_MEM, WIDGET_GPU, WIDGET_FAN,
  WIDGET_BAT, WIDGET_UP, WIDGET_STO, WIDGET_NET, WIDGET_WEATHER, WIDGET_CAL
};

static WidgetType name_to_type(const char *s, int len)
{
  enum { WIDGET_NAME_SZ = 16 };
  if (len >= WIDGET_NAME_SZ)
    len = WIDGET_NAME_SZ - 1;
  char buf[WIDGET_NAME_SZ];
  memcpy(buf, s, len);
  buf[len] = 0;
  for (int i = 0; i < WIDGET_COUNT; i++)
    if (strcasecmp(buf, NAMES[i]) == 0)
      return (WidgetType) i;
  return WIDGET_COUNT;
}

// cppcheck-suppress staticFunction -- exposed for unit testing via widget.h
int widget_parse_list(const char *str, WidgetType *out, int max)
{
  int n = 0;
  if (!str)
    return 0;
  const char *p = str;
  while (*p && n < max) {
    p += strspn(p, ",");
    const char *e = p + strcspn(p, ",");
    if (e > p) {
      WidgetType t = name_to_type(p, e - p);
      if (t < WIDGET_COUNT)
        out[n++] = t;
    }
    p = e;
  }
  return n;
}

int widget_validate(const char *str)
{
  if (!str || !*str)
    return 0;
  const char *p = str;
  while (*p) {
    p += strspn(p, ",");
    const char *e = p + strcspn(p, ",");
    if (e > p) {
      if (name_to_type(p, e - p) >= WIDGET_COUNT)
        return -1;
    }
    p = e;
  }
  return 0;
}

// cppcheck-suppress staticFunction -- exposed for unit testing via widget.h
int widget_default_order(WidgetType *out)
{
  int n = 0;
  for (int i = 0; i < WIDGET_COUNT; i++)
    out[n++] = DEFAULT_ORDER[i];
  return n;
}

// cppcheck-suppress staticFunction -- exposed for unit testing via widget.h
void widget_set_active(struct widget_ctx *ctx, const WidgetType *set, int count)
{
  ctx->active_count = count < WIDGET_COUNT ? count : WIDGET_COUNT;
  ctx->active_mask = 0;
  for (int i = 0; i < ctx->active_count; i++) {
    ctx->active_set[i] = set[i];
    ctx->active_mask |= 1u << set[i];
  }
}

const WidgetType *widget_get_active(const struct widget_ctx *ctx, int *count)
{
  if (count)
    *count = ctx->active_count;
  return ctx->active_set;
}

void widget_setup(struct widget_ctx *ctx, int has_widgets, const char *widgets)
{
  WidgetType wset[WIDGET_COUNT];
  int n;
  if (has_widgets)
    n = widget_parse_list(widgets, wset, WIDGET_COUNT);
  else
    n = widget_default_order(wset);
  widget_set_active(ctx, wset, n);
}
