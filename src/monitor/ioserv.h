#ifndef IOSERV_H
#define IOSERV_H

#include <pthread.h>            // cppcheck-suppress missingIncludeSystem
#include "monitor_int.h"

#define MAX_HTTP_CONNS 3
#define HTTP_RESULT_DATA_SZ 96
#define CANCEL_ALL 0
#define CANCEL_WAN 1
#define CANCEL_WEATHER 2

enum http_state { HTTP_IDLE, HTTP_RUNNING, HTTP_DONE = 2, HTTP_ERROR = -1 };

struct dns_slot;
struct pollfd;
struct async_ctx;

struct http_result {
  pthread_mutex_t lock;
  enum http_state state;
  int cancelled;
  char reason;
  char data[HTTP_RESULT_DATA_SZ];
};

void http_result_write(struct http_result *slot, const char *data, char reason);
void http_result_cancel(struct http_result *slot);
void http_result_clear_cancelled(struct http_result *slot);

struct io_conn {
  int fd;
  int phase;
  struct dns_slot *dns;
  struct http_result *result;
  int *target_fd;
  char req[PATH_SZ];
  int req_len;
  int req_sent;
  char *resp;
  int resp_len;
  int resp_cap;
  int (*parse)(const char *resp, char *out, size_t outsz);
  unsigned char is_wan:1;
};

struct ioserv_ctl {
  pthread_mutex_t lock;
  int efd;
  int io_thread_active;
  int shutdown;
  int cancel_all;
  int wan_cancel;
  int weather_cancel;
  int pending_work;
  struct async_ctx *owner;
  struct io_conn conns[MAX_HTTP_CONNS];
};

int io_init(struct ioserv_ctl *ctl);
void io_cancel_conn(struct io_conn *c);
void io_cancel_all(struct ioserv_ctl *ctl, int kind);
void io_shutdown(struct ioserv_ctl *ctl);
void io_wait_stopped(struct ioserv_ctl *ctl);
void *io_thread_run(void *arg);
int io_setup_conns(struct async_ctx *ctx, struct io_conn *conns);
void io_close_conn(struct io_conn *c);
int io_loop_once(struct ioserv_ctl *ctl, struct io_conn *conns, int n, struct pollfd *pf, struct io_conn **pmap);

#endif
