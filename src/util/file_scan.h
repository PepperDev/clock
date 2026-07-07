#ifndef FILE_SCAN_H
#define FILE_SCAN_H

#include <stddef.h>             // cppcheck-suppress missingIncludeSystem

typedef int (*line_fn_t)(void *ctx, const char *line, int len);

int file_read_lines(const char *path, void *ctx, line_fn_t fn, char *buf, size_t buf_sz);

#endif
