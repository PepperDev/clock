#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <fcntl.h>              // cppcheck-suppress missingIncludeSystem
#include "syscall.h"

#define SLURP_INIT_CAP 1024
#define READ_BUF_SZ 64

int read_file(const char *path, char *b, size_t sz)
{
  int fd = sys_open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  size_t pos = 0;
  ssize_t n;
  while (pos < sz - 1 && (n = sys_read(fd, b + pos, sz - 1 - pos)) > 0)
    pos += (size_t)n;
  sys_close(fd);
  if (pos == 0)
    return -1;
  b[pos] = 0;
  return pos < sz - 1 ? 0 : -1;
}

static int slurp_grow(char **bp, size_t *capp)
{
  if (*capp > (size_t)-1 / 2)
    return -1;
  *capp *= 2;
  char *nb = realloc(*bp, *capp);
  if (!nb)
    return -1;
  *bp = nb;
  return 0;
}

char *slurp(const char *path)
{
  int fd = sys_open(path, O_RDONLY);
  if (fd < 0)
    return NULL;
  size_t cap = SLURP_INIT_CAP;
  char *b = malloc(cap);
  if (!b) {
    sys_close(fd);
    return NULL;
  }
  size_t pos = 0;
  ssize_t n;
  while ((n = sys_read(fd, b + pos, cap - pos - 1)) > 0) {
    pos += (size_t)n, b[pos] = 0;
    if (slurp_grow(&b, &cap) < 0) {
      free(b);
      sys_close(fd);
      return NULL;
    }
  }
  sys_close(fd);
  return b;
}

int read_uint(const char *path, unsigned long long *val)
{
  char b[READ_BUF_SZ];
  if (read_file(path, b, sizeof b) < 0)
    return -1;
  return *val = strtoull(b, NULL, 10), 0;
}
