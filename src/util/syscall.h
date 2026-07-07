#ifndef SYSCALL_H
#define SYSCALL_H

#define weak_alias(old, new) \
  extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))

#include <sys/types.h>          // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <stddef.h>             // cppcheck-suppress missingIncludeSystem
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <glob.h>               // cppcheck-suppress missingIncludeSystem
#include <sys/statvfs.h>        // cppcheck-suppress missingIncludeSystem

#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <sys/sysinfo.h>        // cppcheck-suppress missingIncludeSystem
#include <time.h>               // cppcheck-suppress missingIncludeSystem
int sys_open(const char *path, int flags, ...);
int sys_access(const char *path, int mode);
ssize_t sys_read(int fd, void *buf, size_t count);
ssize_t sys_write(int fd, const void *buf, size_t count);
int sys_close(int fd);
int sys_glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t * pglob);
void sys_globfree(glob_t * pglob);
int sys_statvfs(const char *path, struct statvfs *buf);
int sys_isatty(int fd);
int sys_socket(int domain, int type, int protocol);
int sys_bind(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t sys_sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t sys_recvmsg(int fd, struct msghdr *msg, int flags);
ssize_t sys_send(int fd, const void *buf, size_t len, int flags);
ssize_t sys_recv(int fd, void *buf, size_t len, int flags);
int sys_sysinfo(struct sysinfo *info);
unsigned sys_if_nametoindex(const char *ifname);
char *sys_if_indextoname(unsigned ifindex, char *ifname);
int sys_ioctl(int fd, unsigned long request, void *arg);

#include <poll.h>               // cppcheck-suppress missingIncludeSystem
int sys_poll(struct pollfd *fds, nfds_t nfds, int timeout);

#include <pthread.h>            // cppcheck-suppress missingIncludeSystem
int sys_pthread_create(pthread_t * thread, const pthread_attr_t * attr, void *(*start_routine)(void *), void *arg);

#endif
