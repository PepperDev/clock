#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>

/* ▂▄▆█ in UTF-8 */
static const char BARS[] = {0xe2, 0x96, 0x82, 0xe2, 0x96, 0x84, 0xe2, 0x96, 0x86, 0xe2, 0x96, 0x88, 0};
#define BARS_LEN 12

static int pty_read(int master, char *buf, int max)
{
  int pos = 0;
  while (pos < max - 1) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(master, &rfds);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    int s = select(master + 1, &rfds, NULL, NULL, &tv);
    if (s <= 0) break;
    int n = read(master, buf + pos, max - 1 - pos);
    if (n < 0) {
      if (errno == EIO) break;
      break;
    }
    if (n == 0) break;
    pos += n;
  }
  buf[pos] = 0;
  return pos;
}

static int has_wireless(const char *buf, int len)
{
  return memmem(buf, len, "dBm", 3) != NULL;
}

static int display_cols(const char *s, int max)
{
  int cols = 0;
  for (int i = 0; i < max && s[i]; i++)
    if (((unsigned char)s[i] & 0xC0) != 0x80)
      cols++;
  return cols;
}

static int validate_spacing(const char *buf, int len, const char *desc, int *fail_count)
{
  int fails = 0;
  const char *p = buf;
  int rem = len;
  while ((p = memmem(p, rem, BARS, BARS_LEN)) != NULL) {
    int off = p - buf + BARS_LEN;
    rem = len - off;

    /* find start of line */
    const char *line_start = p;
    while (line_start > buf && line_start[-1] != '\n') line_start--;

    /* find the speed pattern (↓...↑...) before bars, between line_start and p */
    const char *dl = memmem(line_start, p - line_start, "\xe2\x86\x93", 3);
    int ok = 0;
    if (dl && dl < p) {
      /* speed part ends at last char before spaces */
      const char *speed_end = p;
      while (speed_end > dl && (speed_end[-1] == ' ' || speed_end[-1] == '\t'))
        speed_end--;
      int speed_cols = display_cols(dl, speed_end - dl);
      int spaces = (int)(p - speed_end);

      /* total width from ↓ to bars is always 13 columns */
      int expected = 13 - speed_cols;
      if (expected < 1) expected = 1;
      if (expected > 7) expected = 7;
      ok = (spaces == expected);
    }
    if (!ok) {
      fprintf(stderr, "  FAIL: %s - wrong bar spacing near byte %td\n", desc, p - buf);
      fails++;
    }
    p += BARS_LEN;
  }
  *fail_count += fails;
  return fails;
}

static int test_once(const char *clock_bin)
{
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0) { perror("posix_openpt"); return -1; }
  if (grantpt(master) < 0) { perror("grantpt"); close(master); return -1; }
  if (unlockpt(master) < 0) { perror("unlockpt"); close(master); return -1; }
  const char *slave = ptsname(master);
  if (!slave) { perror("ptsname"); close(master); return -1; }

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); close(master); return -1; }

  if (pid == 0) {
    close(master);
    int fd = open(slave, O_RDWR);
    if (fd < 0) _exit(99);
    setsid();
    ioctl(fd, TIOCSCTTY, NULL);
    dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
    if (fd > 2) close(fd);
    execl(clock_bin, "clock", "-o", "-w", "NET", "ascii", NULL);
    _exit(99);
  }

  char out[65536];
  int len = pty_read(master, out, sizeof out);
  close(master);

  int wstatus;
  waitpid(pid, &wstatus, 0);
  if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
    fprintf(stderr, "  FAIL: clock exited abnormally (status %d)\n", wstatus);
    return -1;
  }

  if (len <= 0) {
    fprintf(stderr, "  FAIL: no output\n");
    return -1;
  }

  if (!has_wireless(out, len)) {
    printf("  SKIP: no wireless NIC\n");
    return 0;
  }

  int fails = 0;
  validate_spacing(out, len, "--once", &fails);
  return fails == 0 ? 1 : -1; /* 1 = wireless present and valid */
}

static int count_bars(const char *buf, int len)
{
  int c = 0;
  const char *p = buf;
  int rem = len;
  while ((p = memmem(p, rem, "dBm", 3)) != NULL) {
    c++;
    int adv = p - buf + 3;
    p += 3; rem = len - adv;
  }
  return c;
}

static int drain_nb(int master, char *buf, int max, int target, int *out_got)
{
  int pos = 0;
  int got = 0;
  for (int attempt = 0; attempt < 150 && got < target; attempt++) {
    for (;;) {
      int n = read(master, buf + pos, max - 1 - pos);
      if (n > 0) {
        pos += n;
        buf[pos] = 0;
        got = count_bars(buf, pos);
      } else if (n < 0 && errno == EIO) {
        goto done;
      } else {
        break;
      }
    }
    if (got >= target) break;
    usleep(100000);
  }
done:
  buf[pos] = 0;
  if (out_got) *out_got = got;
  return pos;
}

static int test_continuous(const char *clock_bin)
{
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0) { perror("posix_openpt"); return -1; }
  if (grantpt(master) < 0) { perror("grantpt"); close(master); return -1; }
  if (unlockpt(master) < 0) { perror("unlockpt"); close(master); return -1; }
  const char *slave = ptsname(master);
  if (!slave) { perror("ptsname"); close(master); return -1; }

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); close(master); return -1; }

  if (pid == 0) {
    close(master);
    int fd = open(slave, O_RDWR);
    if (fd < 0) _exit(99);
    setsid();
    ioctl(fd, TIOCSCTTY, NULL);
    dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
    if (fd > 2) close(fd);
    execl(clock_bin, "clock", "-w", "NET", "ascii", NULL);
    _exit(99);
  }

  char out[65536];
  int got = 0;
  int len;
  {
    fcntl(master, F_SETFL, O_NONBLOCK);
    len = drain_nb(master, out, sizeof out, 10, &got);
  }
  kill(pid, SIGTERM);
  close(master);

  int wstatus;
  waitpid(pid, &wstatus, 0);

  if (!has_wireless(out, len)) {
    printf("  SKIP: no wireless in continuous mode\n");
    return 0;
  }

  if (got < 10) {
    fprintf(stderr, "  FAIL: only %d wifi samples (expected >= 10)\n", got);
    return -1;
  }

  int fails = 0;
  validate_spacing(out, len, "continuous", &fails);
  if (fails) return -1;

  printf("  PASS: %d wifi samples validated\n", got);
  return 1;
}

int main(void)
{
  const char *clock_bin = getenv("CLOCK_BIN");
  if (!clock_bin || !clock_bin[0]) clock_bin = "../../bin/clock";

  int rc = 0;
  printf("test_wifi_bars_once...\n");
  int r = test_once(clock_bin);
  if (r < 0) {
    fprintf(stderr, "  FAIL\n");
    rc = 1;
  } else if (r == 0) {
    printf("  SKIP (no wireless NIC)\n");
  } else {
    printf("  PASS\n");
  }

  if (r > 0) {
    printf("test_wifi_bars_continuous...\n");
    int r2 = test_continuous(clock_bin);
    if (r2 < 0) {
      fprintf(stderr, "  FAIL\n");
      rc = 1;
    } else if (r2 == 0) {
      printf("  SKIP (no wireless)\n");
    } else {
      printf("  PASS\n");
    }
  }

  return rc;
}
