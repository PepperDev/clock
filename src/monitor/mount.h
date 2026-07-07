#ifndef MOUNT_H
#define MOUNT_H

#include <stddef.h>             // cppcheck-suppress missingIncludeSystem
#include "monitor_int.h"

#define LOOP_MAJOR 7
#define STATVFS_TICK_MAX 30

struct mount {
  unsigned mnt_id, maj, min;
  char mntpt[PATH_BUF_SZ];
  int consumed;
  unsigned long long total, free;
  int statvfs_tick;
};

struct mount_ctx {
  struct mount *mnts;
  int nm, cap;
};

int mount_read(struct mount_ctx *mc);
struct disk_ctx;
int mount_for_dev(const struct disk_ctx *d, unsigned idx, unsigned maj, unsigned min);
int mount_fmt(char *b, int z, const struct mount *m);

#endif
