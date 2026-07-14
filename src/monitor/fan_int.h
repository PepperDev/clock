#ifndef FAN_INT_H
#define FAN_INT_H

#include "monitor_int.h"

void fan_cache_init(struct cpu_keep *k, const char *dir);
void fan_fb_discover(struct cpu_keep *k);
void copy_entry(struct fentry *e, const char *name, unsigned long long raw);
int ensure_cap(struct fentry **ents, int *cap, int need);
void fmt_inputs(char *l, int *np, const struct fentry *ents, size_t n);

#endif
