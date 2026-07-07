#ifndef ARGPARSE_INT_H
#define ARGPARSE_INT_H

#include "main.h"

void print_usage(void);
int parse_uint(const char *s, int *field);
int parse_short_group(struct args *a, const char *p, const char *n);
int val_flags(struct args *a, const char *p, const char *n);

#endif
