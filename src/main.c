#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE
#include "main.h"

int main(int argc, char **argv)
{
  return clock_main(argc, argv);
}
