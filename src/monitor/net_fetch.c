#define _GNU_SOURCE
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <string.h>             // cppcheck-suppress missingIncludeSystem
#include <stdlib.h>             // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include "util/syscall.h"
#include "monitor_int.h"
#include "monitor.h"

int parse_body(const char *buf, char *ip, size_t sz)
{
  const char *body = strstr(buf, "\r\n\r\n");
  if (!body)
    return -1;
  body += 4 + strspn(body + 4, " \t");
  int blen = (int)strlen(body);
  while (blen > 0 && strchr(" \n\r", body[blen - 1]))
    blen--;
  if (blen < 1 || blen >= (int)sz)
    return -1;
  memcpy(ip, body, blen);
  ip[blen] = 0;
  return 0;
}
