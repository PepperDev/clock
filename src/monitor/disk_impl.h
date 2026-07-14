#ifndef DISK_IMPL_H
#define DISK_IMPL_H

#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"
#include "mount.h"

#define SECTOR_SIZE 512
enum { THR_STR_SZ = 32 };

struct disk_ctx;
unsigned long long sto_delta(unsigned long long cur, unsigned long long *prev);
int read_diskstat(const char *path, unsigned long long *rs, unsigned long long *ws);
int discover_dev(struct disk_ctx *d, const char *path, unsigned *idx);
int dev_read_temp(struct disk_ctx *d, unsigned idx, const char *path);
int sto_fmt_thr(char *b, int z, unsigned long long v);
#endif
