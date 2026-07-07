#define _GNU_SOURCE
#include "mock_syscall.h"
#include "util/syscall.h"
#include <stdio.h>              // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <string.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <stdarg.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <glob.h>               // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <linux/netlink.h>      // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <sys/ioctl.h>          // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <net/if.h>             // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <netinet/in.h>         // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <arpa/inet.h>          // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <linux/ethtool.h>      // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <linux/sockios.h>      // cppcheck-suppress missingIncludeSystem -- SIOCETHTOOL
#include <poll.h>               // cppcheck-suppress missingIncludeSystem
#include <errno.h>              // cppcheck-suppress missingIncludeSystem

extern ssize_t __real_sys_send(int fd, const void *buf, size_t len, int flags);
extern ssize_t __real_sys_recv(int fd, void *buf, size_t len, int flags);

#define IFACE_NAME_LEN 24

#define MAX_MOCK_FILES 48
#define MAX_MOCK_GLOBS 32
#define MAX_MOCK_STATVFS 8
#define MOCK_WRITE_BUF 65536

struct mock_file_entry {
  char path[128];
  char content[4096];
  size_t len;
};

static struct mock_file_entry mock_files[MAX_MOCK_FILES];
static int mock_file_count = 0;
static size_t mock_read_pos[MAX_MOCK_FILES];

struct mock_glob_entry {
  char pattern[128];
  char *results[16];
  int count;
};

static struct mock_glob_entry mock_globs[MAX_MOCK_GLOBS];
static int mock_glob_count = 0;

struct mock_statvfs_entry {
  char path[128];
  struct statvfs st;
};

static struct mock_statvfs_entry mock_statvfs_entries[MAX_MOCK_STATVFS];
static int mock_statvfs_count = 0;

static char mock_write_buf[MOCK_WRITE_BUF];
static size_t mock_write_len = 0;

static struct mock_file_entry *mock_file_find(const char *path)
{
  for (int i = 0; i < mock_file_count; i++)
    if (strcmp(mock_files[i].path, path) == 0)
      return &mock_files[i];
  return NULL;
}

void mock_file(const char *path, const char *content)
{
  struct mock_file_entry *e = mock_file_find(path);
  if (!e) {
    if (mock_file_count >= MAX_MOCK_FILES)
      return;
    e = &mock_files[mock_file_count++];
    strncpy(e->path, path, sizeof e->path - 1);
    e->path[sizeof e->path - 1] = 0;
  }
  e->len = strlen(content);
  if (e->len > sizeof e->content - 1)
    e->len = sizeof e->content - 1;
  memcpy(e->content, content, e->len);
  e->content[e->len] = 0;
}

void mock_glob(const char *pattern, char *results[], int count)
{
  if (mock_glob_count >= MAX_MOCK_GLOBS)
    return;
  struct mock_glob_entry *e = &mock_globs[mock_glob_count++];
  strncpy(e->pattern, pattern, sizeof e->pattern - 1);
  e->pattern[sizeof e->pattern - 1] = 0;
  e->count = count < 16 ? count : 16;
  for (int i = 0; i < e->count; i++)
    e->results[i] = results[i];
}

const char *mock_get_output(void)
{
  mock_write_buf[mock_write_len] = 0;
  return mock_write_buf;
}

void mock_statvfs(const char *path, const struct statvfs *st)
{
  if (mock_statvfs_count >= MAX_MOCK_STATVFS)
    return;
  struct mock_statvfs_entry *e = &mock_statvfs_entries[mock_statvfs_count++];
  strncpy(e->path, path, sizeof e->path - 1);
  e->path[sizeof e->path - 1] = 0;
  e->st = *st;
}

/* netlink mock */
static int mock_nl_fd = -1;
static int mock_nl_call;
#define MOCK_NL_MAX 24
static const void *mock_nl_resp[MOCK_NL_MAX];
static size_t mock_nl_len[MOCK_NL_MAX];
static int mock_nl_count;

static int mock_sysinfo_ret;
static struct sysinfo mock_sysinfo_val;

void mock_set_netlink(int socket_fd, const void *resp1, size_t len1, const void *resp2, size_t len2)
{
  mock_nl_fd = socket_fd;
  mock_nl_call = 0;
  mock_nl_count = 0;
  if (resp1) {
    mock_nl_resp[0] = resp1;
    mock_nl_len[0] = len1;
    mock_nl_count = 1;
  }
  if (resp2) {
    mock_nl_resp[1] = resp2;
    mock_nl_len[1] = len2;
    mock_nl_count = 2;
  }
}

void mock_add_nl_resp(const void *resp, size_t len)
{
  if (mock_nl_count < MOCK_NL_MAX) {
    mock_nl_resp[mock_nl_count] = resp;
    mock_nl_len[mock_nl_count] = len;
    mock_nl_count++;
  }
}

int sys_socket(int domain, int type, int protocol)
{
  (void)domain;
  (void)type;
  (void)protocol;
  return mock_nl_fd;
}

int sys_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
  (void)fd;
  (void)addr;
  (void)len;
  return mock_nl_fd >= 0 ? 0 : -1;
}

ssize_t sys_sendmsg(int fd, const struct msghdr *msg, int flags)
{
  (void)fd;
  (void)msg;
  (void)flags;
  return mock_nl_fd >= 0 ? (ssize_t) ((const struct nlmsghdr *)msg->msg_iov[0].iov_base)->nlmsg_len : -1;
}

ssize_t sys_recvmsg(int fd, struct msghdr *msg, int flags)
{
  (void)fd;
  (void)flags;
  if (mock_nl_count <= 0 || mock_nl_fd < 0)
    return -1;
  int i = mock_nl_call < mock_nl_count ? mock_nl_call : mock_nl_count - 1;
  mock_nl_call++;
  if (!mock_nl_resp[i])
    return -1;
  size_t cpy = mock_nl_len[i] < (size_t)msg->msg_iov[0].iov_len ? mock_nl_len[i] : (size_t)msg->msg_iov[0].iov_len;
  memcpy(msg->msg_iov[0].iov_base, mock_nl_resp[i], cpy);
  return (ssize_t) cpy;
}

ssize_t sys_send(int fd, const void *buf, size_t len, int flags)
{
  return __real_sys_send(fd, buf, len, flags);
}

ssize_t sys_recv(int fd, void *buf, size_t len, int flags)
{
  return __real_sys_recv(fd, buf, len, flags);
}

int sys_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
  (void)fds;
  (void)nfds;
  (void)timeout;
  return 1;                     /* indicate data ready */
}

int sys_sysinfo(struct sysinfo *info)
{
  if (!info)
    return -1;
  *info = mock_sysinfo_val;
  return mock_sysinfo_ret;
}

void mock_set_sysinfo(const struct sysinfo *si, int ret)
{
  if (si)
    mock_sysinfo_val = *si;
  mock_sysinfo_ret = ret;
}

static unsigned mock_n2i_val;

void mock_set_ifindex(unsigned idx)
{
  mock_n2i_val = idx;
}

#define MAX_IFNAME 8
static struct {
  unsigned idx;
  char name[IFACE_NAME_LEN];
} mock_ifnames[MAX_IFNAME];
static int mock_ifname_count;

void mock_set_ifname(unsigned idx, const char *name)
{
  if (mock_ifname_count >= MAX_IFNAME)
    return;
  mock_ifnames[mock_ifname_count].idx = idx;
  snprintf(mock_ifnames[mock_ifname_count].name, IFACE_NAME_LEN, "%s", name);
  mock_ifname_count++;
}

#define MAX_MOCK_SPEED 8
static struct {
  char iface[IFACE_NAME_LEN];
  int speed;
} mock_speeds[MAX_MOCK_SPEED];
static int mock_speed_count;

static struct {
  char iface[IFACE_NAME_LEN];
  int speed;
} mock_gset_speeds[MAX_MOCK_SPEED];
static int mock_gset_speed_count;
int mock_no_glinksettings;      /* non-static, used by monitor_test.c */

void mock_set_link_speed(const char *iface, int speed)
{
  if (mock_speed_count >= MAX_MOCK_SPEED)
    return;
  snprintf(mock_speeds[mock_speed_count].iface, IFACE_NAME_LEN, "%s", iface);
  mock_speeds[mock_speed_count].speed = speed;
  mock_speed_count++;
}

void mock_set_gset_speed(const char *iface, int speed)
{
  if (mock_gset_speed_count >= MAX_MOCK_SPEED)
    return;
  snprintf(mock_gset_speeds[mock_gset_speed_count].iface, IFACE_NAME_LEN, "%s", iface);
  mock_gset_speeds[mock_gset_speed_count].speed = speed;
  mock_gset_speed_count++;
}

unsigned sys_if_nametoindex(const char *ifname)
{
  for (int i = 0; i < mock_ifname_count; i++) {
    if (strcmp(mock_ifnames[i].name, ifname) == 0)
      return mock_ifnames[i].idx;
  }
  return mock_n2i_val;
}

char *sys_if_indextoname(unsigned ifindex, char *ifname)
{
  if (ifindex < 1)
    return NULL;
  for (int i = 0; i < mock_ifname_count; i++) {
    if (mock_ifnames[i].idx == ifindex) {
      snprintf(ifname, IFACE_NAME_LEN, "%s", mock_ifnames[i].name);
      return ifname;
    }
  }
  snprintf(ifname, IFACE_NAME_LEN, "eth%u", ifindex);
  return ifname;
}

int sys_isatty(int fd)
{
  (void)fd;
  return 0;
}

int sys_access(const char *path, int mode)
{
  (void)mode;
  for (int i = 0; i < mock_file_count; i++) {
    if (strcmp(mock_files[i].path, path) == 0)
      return 0;
  }
  for (int i = 0; i < mock_file_count; i++) {
    size_t plen = strlen(path);
    if (strncmp(mock_files[i].path, path, plen) == 0 && mock_files[i].path[plen] == '/')
      return 0;
  }
  for (int i = 0; i < mock_glob_count; i++) {
    size_t plen = strlen(path);
    const char *pattern = mock_globs[i].pattern;
    if (strncmp(pattern, path, plen) == 0 && pattern[plen] == '/')
      return 0;
  }
  return -1;
}

int mock_pthread_create_fail;
int mock_syscall_real_threads;

/* Sentinel for fake thread handles — must be writable and large enough
   that pthread_cancel/pthread_detach don't crash when accessing fields
   (at ~0x3c offset for canceldisable on musl x86_64). */
static char sentinel_thread[512];

int sys_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
  (void)attr;
  if (mock_pthread_create_fail)
    return -1;
  if (mock_syscall_real_threads)
    return pthread_create(thread, attr, start_routine, arg);
  (void)start_routine;
  (void)arg;
  *thread = (pthread_t) sentinel_thread;
  return 0;
}

void mock_reset(void)
{
  mock_file_count = 0;
  mock_glob_count = 0;
  mock_statvfs_count = 0;
  mock_write_len = 0;
  mock_nl_fd = -1;
  mock_nl_call = 0;
  mock_nl_count = 0;
  for (int i = 0; i < MOCK_NL_MAX; i++) {
    mock_nl_resp[i] = NULL;
    mock_nl_len[i] = 0;
  }
  mock_n2i_val = 0;
  mock_ifname_count = 0;
  mock_sysinfo_ret = 0;
  memset(&mock_sysinfo_val, 0, sizeof mock_sysinfo_val);
  mock_speed_count = 0;
  mock_gset_speed_count = 0;
  mock_no_glinksettings = 0;
  mock_pthread_create_fail = 0;
  mock_syscall_real_threads = 0;
}

int sys_open(const char *path, int flags, ...)
{
  (void)flags;
  int idx = -1;
  for (int i = 0; i < mock_file_count; i++) {
    if (strcmp(mock_files[i].path, path) == 0)
      idx = i;
  }
  if (idx >= 0) {
    mock_read_pos[idx] = 0;
    return idx + 10;
  }
  return -1;
}

ssize_t sys_read(int fd, void *buf, size_t count)
{
  int idx = fd - 10;
  if (idx < 0 || idx >= mock_file_count)
    return -1;
  const struct mock_file_entry *e = &mock_files[idx];
  size_t pos = mock_read_pos[idx];
  if (pos >= e->len)
    return 0;
  size_t avail = e->len - pos;
  if (count > avail)
    count = avail;
  memcpy(buf, e->content + pos, count);
  mock_read_pos[idx] = pos + count;
  return count;
}

ssize_t sys_write(int fd, const void *buf, size_t count)
{
  (void)fd;
  if (mock_write_len + count > MOCK_WRITE_BUF)
    count = MOCK_WRITE_BUF - mock_write_len;
  memcpy(mock_write_buf + mock_write_len, buf, count);
  mock_write_len += count;
  return count;
}

int sys_close(int fd)
{
  (void)fd;
  return 0;
}

int sys_glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob)
{
  (void)flags;
  (void)errfunc;
  for (int i = 0; i < mock_glob_count; i++) {
    if (strcmp(mock_globs[i].pattern, pattern) == 0) {
      pglob->gl_pathc = mock_globs[i].count;
      pglob->gl_pathv = mock_globs[i].results;
      pglob->gl_offs = 0;
      return 0;
    }
  }
  return GLOB_NOMATCH;
}

void sys_globfree(glob_t *pglob)
{
  (void)pglob;
}

int sys_statvfs(const char *path, struct statvfs *buf)
{
  for (int i = 0; i < mock_statvfs_count; i++) {
    if (strcmp(mock_statvfs_entries[i].path, path) == 0) {
      *buf = mock_statvfs_entries[i].st;
      return 0;
    }
  }
  return -1;
}

static int mock_ethtool_speed(struct ifreq *ifr)
{
  struct ethtool_link_settings *p = (struct ethtool_link_settings *)ifr->ifr_data;
  if (p->cmd == ETHTOOL_GSET) {
    for (int i = 0; i < mock_gset_speed_count; i++) {
      if (strcmp(mock_gset_speeds[i].iface, ifr->ifr_name) == 0) {
        struct ethtool_cmd *ecmd = (struct ethtool_cmd *)ifr->ifr_data;
        int spd = mock_gset_speeds[i].speed;
        ecmd->speed = (__u16) (spd & 0xFFFF);
        ecmd->speed_hi = (__u16) ((unsigned)spd >> 16);
        return 0;
      }
    }
    return -1;
  }
  if (p->cmd != ETHTOOL_GLINKSETTINGS)
    return -1;
  if (p->link_mode_masks_nwords == 0) {
    if (mock_no_glinksettings)
      return -1;
    p->link_mode_masks_nwords = 4;
    return 0;
  }
  for (int i = 0; i < mock_speed_count; i++) {
    if (strcmp(mock_speeds[i].iface, ifr->ifr_name) == 0) {
      p->speed = (unsigned)mock_speeds[i].speed;
      return 0;
    }
  }
  return -1;
}

int sys_ioctl(int fd, unsigned long request, void *arg)
{
  (void)fd;
  if (request == SIOCGIFADDR) {
    struct ifreq *ifr = arg;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "10.0.0.1", &sin->sin_addr);
    return 0;
  }
  if (request == SIOCETHTOOL)
    return mock_ethtool_speed((struct ifreq *)arg);
  return -1;
}
