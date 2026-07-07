#define _GNU_SOURCE
#include "util/syscall.h"
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <fcntl.h>              // cppcheck-suppress missingIncludeSystem
#include <stdarg.h>             // cppcheck-suppress missingIncludeSystem
#include <glob.h>               // cppcheck-suppress missingIncludeSystem
#include <sys/statvfs.h>        // cppcheck-suppress missingIncludeSystem
#include <sys/ioctl.h>          // cppcheck-suppress missingIncludeSystem
#include <net/if.h>             // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
#include <poll.h>               // cppcheck-suppress missingIncludeSystem

// cppcheck-suppress staticFunction -- referenced by weak_alias
int __real_sys_open(const char *path, int flags, ...)
{
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
  }
  return open(path, flags, mode);
}

weak_alias(__real_sys_open, sys_open);

// cppcheck-suppress staticFunction -- referenced by weak_alias
int __real_sys_access(const char *path, int mode)
{
  return access(path, mode);
}

weak_alias(__real_sys_access, sys_access);

// cppcheck-suppress staticFunction -- referenced by weak_alias
ssize_t __real_sys_read(int fd, void *buf, size_t count)
{
  return read(fd, buf, count);
}

weak_alias(__real_sys_read, sys_read);

// cppcheck-suppress staticFunction -- referenced by weak_alias
ssize_t __real_sys_write(int fd, const void *buf, size_t count)
{
  return write(fd, buf, count);
}

weak_alias(__real_sys_write, sys_write);

// cppcheck-suppress staticFunction -- referenced by weak_alias
int __real_sys_close(int fd)
{
  return close(fd);
}

weak_alias(__real_sys_close, sys_close);

// cppcheck-suppress staticFunction -- referenced by weak_alias
int __real_sys_glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob)
{
  return glob(pattern, flags, errfunc, pglob);
}

weak_alias(__real_sys_glob, sys_glob);

// cppcheck-suppress staticFunction -- referenced by weak_alias
void __real_sys_globfree(glob_t *pglob)
{
  globfree(pglob);
}

weak_alias(__real_sys_globfree, sys_globfree);

// cppcheck-suppress staticFunction -- referenced by weak_alias
int __real_sys_statvfs(const char *path, struct statvfs *buf)
{
  return statvfs(path, buf);
}

weak_alias(__real_sys_statvfs, sys_statvfs);

// cppcheck-suppress staticFunction -- referenced by weak_alias
int __real_sys_isatty(int fd)
{
  return isatty(fd);
}

weak_alias(__real_sys_isatty, sys_isatty);

// cppcheck-suppress staticFunction -- referenced by weak_alias
int __real_sys_socket(int domain, int type, int protocol)
{
  return socket(domain, type, protocol);
}

weak_alias(__real_sys_socket, sys_socket);

// cppcheck-suppress staticFunction -- referenced by weak_alias
int __real_sys_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
  return bind(fd, addr, len);
}

weak_alias(__real_sys_bind, sys_bind);

// cppcheck-suppress staticFunction -- referenced by weak_alias
ssize_t __real_sys_sendmsg(int fd, const struct msghdr *msg, int flags)
{
  return sendmsg(fd, msg, flags);
}

weak_alias(__real_sys_sendmsg, sys_sendmsg);

// cppcheck-suppress staticFunction -- referenced by weak_alias
ssize_t __real_sys_recvmsg(int fd, struct msghdr *msg, int flags)
{
  return recvmsg(fd, msg, flags);
}

weak_alias(__real_sys_recvmsg, sys_recvmsg);

// cppcheck-suppress staticFunction -- referenced by weak_alias
ssize_t __real_sys_send(int fd, const void *buf, size_t len, int flags)
{
  return send(fd, buf, len, flags);
}

weak_alias(__real_sys_send, sys_send);

// cppcheck-suppress staticFunction -- referenced by weak_alias
ssize_t __real_sys_recv(int fd, void *buf, size_t len, int flags)
{
  return recv(fd, buf, len, flags);
}

weak_alias(__real_sys_recv, sys_recv);

// cppcheck-suppress staticFunction -- referenced by weak_alias
int __real_sys_sysinfo(struct sysinfo *info)
{
  return sysinfo(info);
}

weak_alias(__real_sys_sysinfo, sys_sysinfo);

// cppcheck-suppress staticFunction -- referenced by weak_alias
unsigned __real_sys_if_nametoindex(const char *ifname)
{
  return if_nametoindex(ifname);
}

weak_alias(__real_sys_if_nametoindex, sys_if_nametoindex);

// cppcheck-suppress staticFunction -- referenced by weak_alias
char *__real_sys_if_indextoname(unsigned ifindex, char *ifname)
{
  return if_indextoname(ifindex, ifname);
}

weak_alias(__real_sys_if_indextoname, sys_if_indextoname);

// cppcheck-suppress staticFunction -- referenced by weak_alias
int __real_sys_ioctl(int fd, unsigned long request, void *arg)
{
  return ioctl(fd, request, arg);
}

weak_alias(__real_sys_ioctl, sys_ioctl);

// cppcheck-suppress staticFunction -- referenced by weak_alias
int __real_sys_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
  return poll(fds, nfds, timeout);
}

weak_alias(__real_sys_poll, sys_poll);

// cppcheck-suppress staticFunction -- referenced by weak_alias
int __real_sys_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
  return pthread_create(thread, attr, start_routine, arg);
}

weak_alias(__real_sys_pthread_create, sys_pthread_create);
