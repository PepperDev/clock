#define _GNU_SOURCE
#include <unistd.h>             // cppcheck-suppress missingIncludeSystem
#include <sys/eventfd.h>        // cppcheck-suppress missingIncludeSystem
#include "ioserv.h"
#include "util/syscall.h"

int io_init(struct ioserv_ctl *ctl)
{
  ctl->efd = -1;
  ctl->io_thread_active = 0;
  ctl->shutdown = 0;
  ctl->cancel_all = 0;
  ctl->wan_cancel = 0;
  ctl->weather_cancel = 0;
  ctl->pending_work = 0;
  ctl->lock = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;
  for (int i = 0; i < MAX_HTTP_CONNS; i++)
    ctl->conns[i].fd = -1;
  return 0;
}

void io_cancel_conn(struct io_conn *c)
{
  if (c->fd >= 0) {
    sys_close(c->fd);
    c->fd = -1;
  }
  if (c->target_fd)
    __atomic_store_n(c->target_fd, -1, __ATOMIC_RELEASE);
}

void io_cancel_all(struct ioserv_ctl *ctl, int kind)
{
  pthread_mutex_lock(&ctl->lock);
  if (kind == CANCEL_WAN)
    ctl->wan_cancel = 1;
  else if (kind == CANCEL_WEATHER)
    ctl->weather_cancel = 1;
  else
    ctl->cancel_all = 1;
  for (int i = 0; i < MAX_HTTP_CONNS; i++) {
    struct io_conn *c = &ctl->conns[i];
    if (kind && (kind == 1) != c->is_wan)
      continue;
    io_cancel_conn(c);
  }
  pthread_mutex_unlock(&ctl->lock);
}

void io_shutdown(struct ioserv_ctl *ctl)
{
  pthread_mutex_lock(&ctl->lock);
  ctl->shutdown = 1;
  int efd = ctl->efd;
  ctl->efd = -1;
  for (int i = 0; i < MAX_HTTP_CONNS; i++) {
    struct io_conn *c = &ctl->conns[i];
    if (c->fd >= 0) {
      sys_close(c->fd);
      c->fd = -1;
    }
  }
  pthread_mutex_unlock(&ctl->lock);
  if (efd >= 0)
    sys_close(efd);
}

void io_wait_stopped(struct ioserv_ctl *ctl)
{
  for (;;) {
    pthread_mutex_lock(&ctl->lock);
    int active = ctl->io_thread_active;
    pthread_mutex_unlock(&ctl->lock);
    if (!active)
      return;
    sched_yield();
  }
}
