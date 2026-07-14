#ifndef MOCK_SYSCALL_H
#define MOCK_SYSCALL_H

#include <sys/statvfs.h>        // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem -- musl include paths not known
#include <sys/sysinfo.h>        // cppcheck-suppress missingIncludeSystem -- musl include paths not known

void mock_file(const char *path, const char *content);
void mock_glob(const char *pattern, char *results[], int count);
void mock_statvfs(const char *path, const struct statvfs *st);
const char *mock_get_output(void);
void mock_reset(void);

void mock_set_netlink(int socket_fd, const void *resp1, size_t len1, const void *resp2, size_t len2);
void mock_add_nl_resp(const void *resp, size_t len);
void mock_set_sysinfo(const struct sysinfo *si, int ret);
void mock_set_ifindex(unsigned idx);
void mock_set_ifname(unsigned idx, const char *name);
void mock_set_link_speed(const char *iface, int speed);
void mock_set_gset_speed(const char *iface, int speed);
extern int mock_no_glinksettings;
extern int mock_pthread_create_fail;
extern int mock_syscall_real_threads;
extern int mock_sys_open_count;
extern int mock_sys_socket_type;
extern int mock_sys_close_fd;

#endif
