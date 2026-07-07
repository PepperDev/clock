#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include "file_scan.h"
#include "syscall.h"

static int process_chunk(void *ctx, line_fn_t fn, char *buf, int *s)
{
  char *nl;
  while ((nl = memchr(buf, '\n', *s))) {
    int len = (int)(nl - buf);
    *nl = 0;
    if (fn(ctx, buf, len))
      return -1;
    *s -= len + 1;
    memmove(buf, nl + 1, *s);
  }
  return 0;
}

static int drain_line(void *ctx, line_fn_t fn, char *buf, int s)
{
  return s && fn(ctx, buf, s) ? -1 : 0;
}

int file_read_lines(const char *path, void *ctx, line_fn_t fn, char *buf, size_t buf_sz)
{
  int fd = sys_open(path, 0);
  if (fd < 0)
    return -1;
  int s = 0, rc = 0;
  ssize_t n;
  while ((n = sys_read(fd, buf + s, buf_sz - s)) > 0) {
    s += n;
    if (process_chunk(ctx, fn, buf, &s))
      goto done;
  }
  rc = n < 0 ? -1 : drain_line(ctx, fn, buf, s);
 done:
  sys_close(fd);
  return rc;
}
