#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <errno.h>

static int test_once_pty(const char *clock_bin)
{
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0) { perror("posix_openpt"); return 1; }
  if (grantpt(master) < 0) { perror("grantpt"); close(master); return 1; }
  if (unlockpt(master) < 0) { perror("unlockpt"); close(master); return 1; }
  const char *slave = ptsname(master);
  if (!slave) { perror("ptsname"); close(master); return 1; }

  /* save original slave termios before child */
  int sfd = open(slave, O_RDWR | O_NOCTTY);
  if (sfd < 0) { perror("open slave"); close(master); return 1; }
  struct termios orig;
  tcgetattr(sfd, &orig);
  /* verify echo is set before (pty default) */
  if (!(orig.c_lflag & ECHO)) {
    fprintf(stderr, "FAIL: expected ECHO set before clock\n");
    close(sfd); close(master); return 1;
  }
  close(sfd);

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); close(master); return 1; }

  if (pid == 0) {
    /* child: set up pty slave */
    close(master);
    int fd = open(slave, O_RDWR);
    if (fd < 0) _exit(99);
    setsid();
    ioctl(fd, TIOCSCTTY, NULL);
    dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
    if (fd > 2) close(fd);
    execl(clock_bin, "clock", "-oa", "ascii", NULL);
    _exit(99);
  }

  /* parent: wait for child, then check terminal */
  int wstatus;
  pid_t w = waitpid(pid, &wstatus, 0);
  if (w < 0) { perror("waitpid"); close(master); return 1; }

  /* re-open slave (child may have closed) */
  sfd = open(slave, O_RDWR | O_NOCTTY);
  if (sfd < 0) { perror("open slave after"); close(master); return 1; }
  struct termios after;
  tcgetattr(sfd, &after);
  int ok = 1;
  /* echo must be restored on clean exit */
  if (!(after.c_lflag & ECHO)) {
    fprintf(stderr, "FAIL: ECHO not restored after clock exit\n");
    ok = 0;
  }
  /* all other lflag bits should match original */
  tcflag_t lflag_mask = ECHO | ECHOE | ECHOK | ECHONL | ICANON | ISIG | IEXTEN;
  if ((after.c_lflag & lflag_mask) != (orig.c_lflag & lflag_mask)) {
    fprintf(stderr, "FAIL: lflag mismatch after=0x%x orig=0x%x\n",
            (unsigned)after.c_lflag, (unsigned)orig.c_lflag);
    ok = 0;
  }
  close(sfd);
  close(master);

  if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
    fprintf(stderr, "FAIL: clock exited abnormally (status %d)\n", wstatus);
    return 1;
  }
  return ok ? 0 : 1;
}

static int test_abnormal_pty(const char *clock_bin)
{
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0) { perror("posix_openpt"); return 1; }
  if (grantpt(master) < 0) { perror("grantpt"); close(master); return 1; }
  if (unlockpt(master) < 0) { perror("unlockpt"); close(master); return 1; }
  const char *slave = ptsname(master);
  if (!slave) { perror("ptsname"); close(master); return 1; }

  int sfd = open(slave, O_RDWR | O_NOCTTY);
  if (sfd < 0) { perror("open slave"); close(master); return 1; }
  struct termios orig;
  tcgetattr(sfd, &orig);
  close(sfd);

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); close(master); return 1; }

  if (pid == 0) {
    close(master);
    int fd = open(slave, O_RDWR);
    if (fd < 0) _exit(99);
    setsid();
    ioctl(fd, TIOCSCTTY, NULL);
    dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
    if (fd > 2) close(fd);
    execl(clock_bin, "clock", "-a", "ascii", NULL);
    _exit(99);
  }

  /* let clock run for 2s, then terminate it */
  usleep(2000000);
  kill(pid, SIGTERM);

  /* wait for cleanup to write \033[?25h */
  usleep(500000);

  /* drain all output from pty master and check for restoration sequence */
  char buf[65536];
  int pos = 0;
  for (;;) {
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(master, &rfds);
    int r = select(master + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) break;
    int n = read(master, buf + pos, 65535 - pos);
    if (n <= 0) break;
    pos += n;
  }
  buf[pos] = 0;
  int found = memmem(buf, pos, "\033[?25h", 6) != NULL;

  sfd = open(slave, O_RDWR | O_NOCTTY);
  if (sfd < 0) { perror("open slave after"); close(master); return 1; }
  struct termios after;
  tcgetattr(sfd, &after);
  int ok = 1;
  if (!(after.c_lflag & ECHO)) {
    fprintf(stderr, "FAIL: ECHO not restored after abnormal termination\n");
    ok = 0;
  }
  tcflag_t lflag_mask = ECHO | ECHOE | ECHOK | ECHONL | ICANON | ISIG | IEXTEN;
  if ((after.c_lflag & lflag_mask) != (orig.c_lflag & lflag_mask)) {
    fprintf(stderr, "FAIL: lflag mismatch after abnormal term\n");
    ok = 0;
  }
  if (!found) {
    fprintf(stderr, "FAIL: restoration sequence \\033[?25h not found after SIGTERM\n");
    ok = 0;
  }
  close(sfd);
  close(master);

  int wstatus;
  waitpid(pid, &wstatus, 0);
  return ok ? 0 : 1;
}

int main(void)
{
  const char *clock_bin = getenv("CLOCK_BIN");
  if (!clock_bin || !clock_bin[0]) clock_bin = "../../bin/clock";

  int rc = 0;
  printf("test_once_pty...\n");
  if (test_once_pty(clock_bin)) {
    fprintf(stderr, "  FAIL: TTY restoration test (--once)\n");
    rc = 1;
  } else {
    printf("  PASS\n");
  }

  printf("test_abnormal_pty...\n");
  if (test_abnormal_pty(clock_bin)) {
    fprintf(stderr, "  FAIL: TTY abnormal termination test\n");
    rc = 1;
  } else {
    printf("  PASS\n");
  }

  return rc;
}
