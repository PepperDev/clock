#ifndef WIDGET_H
#define WIDGET_H

typedef enum {
  WIDGET_DATE,
  WIDGET_CPU,
  WIDGET_GPU,
  WIDGET_MEM,
  WIDGET_FAN,
  WIDGET_BAT,
  WIDGET_UP,
  WIDGET_STO,
  WIDGET_NET,
  WIDGET_WEATHER,
  WIDGET_CAL,
  WIDGET_COUNT
} WidgetType;

struct widget_ctx {
  WidgetType active_set[WIDGET_COUNT];
  unsigned int active_mask;
  int active_count;
};

int widget_validate(const char *str);
int widget_parse_list(const char *str, WidgetType * out, int max);
int widget_default_order(WidgetType * out, int gpu, int fan);
void widget_set_active(struct widget_ctx *ctx, const WidgetType * set, int count);
const WidgetType *widget_get_active(const struct widget_ctx *ctx, int *count);
void widget_setup(struct widget_ctx *ctx, int has_widgets, const char *widgets, int gpu, int fan);

static inline int widget_active(const struct widget_ctx *ctx, WidgetType t)
{
  return (ctx->active_mask >> t) & 1u;
}

#endif
