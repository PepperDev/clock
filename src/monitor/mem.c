#define _GNU_SOURCE
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem -- sscanf
#include "util/file_scan.h"
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

#define CGROUP_UNLIMITED (1ULL << 50)

static void try_total(const char *p, size_t len, unsigned long long *total)
{
  if (len > 9 && memcmp(p, "MemTotal:", 9) == 0)
    sscanf(p, "MemTotal:%llu", total);
}

static void try_avail(const char *p, size_t len, unsigned long long *avail)
{
  if (len > 12 && memcmp(p, "MemAvailable:", 13) == 0)
    sscanf(p, "MemAvailable:%llu", avail);
}

struct mem_ctx {
  unsigned long long *total, *avail;
};

static int mem_line(void *ctx, const char *line, int len)
{
  struct mem_ctx *mc = ctx;
  try_total(line, len, mc->total);
  try_avail(line, len, mc->avail);
  return *mc->total && *mc->avail ? 1 : 0;
}

static void read_mem_values(unsigned long long *total, unsigned long long *avail)
{
  char buf[BIG_BUF];
  struct mem_ctx mc = { total, avail };
  file_read_lines("/proc/meminfo", &mc, mem_line, buf, sizeof buf);
}

void mem_usage(struct clock_state *ci)
{
  unsigned long long total = 0, avail = 0;
  read_mem_values(&total, &avail);
  ci->mem_total_kb = total;
  ci->mem_avail_kb = avail;
  ci->mem_pct = total > 0 ? (int)((total - avail) * PERCENT_BASE / total) : 0;
}

static int cgroup_mem_pair(const char *usage, const char *limit, unsigned long long *used_kb,
                           unsigned long long *max_kb)
{
  unsigned long long uv, lv;
  if (read_uint(usage, &uv) != 0)
    return -1;
  *used_kb = uv / BYTES_PER_KB;
  if (read_uint(limit, &lv) != 0 || lv == 0 || lv >= CGROUP_UNLIMITED)
    return 0;
  *max_kb = lv / BYTES_PER_KB;
  return 1;
}

#define MEM_NO_CGROUP_V2 (1u << 0)
#define MEM_NO_CGROUP_V1 (1u << 1)

void mem_discover(struct cpu_keep *k)
{
  if (sys_access("/sys/fs/cgroup/memory.current", F_OK) == 0)
    return;
  k->mem_flags |= MEM_NO_CGROUP_V2;
  if (sys_access("/sys/fs/cgroup/memory/memory.usage_in_bytes", F_OK) == 0)
    return;
  k->mem_flags |= MEM_NO_CGROUP_V1;
}

void get_container_mem(struct clock_state *ci)
{
  const struct cpu_keep *k = &ci->keep;
  int ret = -1;
  if (!(k->mem_flags & MEM_NO_CGROUP_V2))
    ret = cgroup_mem_pair("/sys/fs/cgroup/memory.current",
                          "/sys/fs/cgroup/memory.max", &ci->ctr_used_kb, &ci->ctr_max_kb);
  if (ret < 0 && !(k->mem_flags & MEM_NO_CGROUP_V1))
    ret = cgroup_mem_pair("/sys/fs/cgroup/memory/memory.usage_in_bytes",
                          "/sys/fs/cgroup/memory/memory.limit_in_bytes", &ci->ctr_used_kb, &ci->ctr_max_kb);
  if (ret == 0) {
    ci->ctr_max_kb = ci->mem_total_kb;
    ret = 1;
  }
  if (ret > 0) {
    ci->ctr_pct = (int)(ci->ctr_used_kb * (unsigned long long)PERCENT_BASE / ci->ctr_max_kb);
    ci->has_ctr = 1;
  }
}
