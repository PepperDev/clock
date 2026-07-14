#ifndef MONITOR_INT_H
#define MONITOR_INT_H

#include <stdio.h>              // cppcheck-suppress missingIncludeSystem
#include <stddef.h>             // cppcheck-suppress missingIncludeSystem
#include <sys/socket.h>         // cppcheck-suppress missingIncludeSystem
#include <sys/sysinfo.h>        // cppcheck-suppress missingIncludeSystem
#include <pthread.h>            // cppcheck-suppress missingIncludeSystem
#include <netdb.h>              // cppcheck-suppress missingIncludeSystem
#include "widget.h"

#define SECS_PER_DAY 86400
#define SECS_PER_HOUR 3600
#define SECS_PER_MIN 60
#define IFACE_NAME_LEN 24
#define IFACE_NAME_FMT "%23s"
#define KHZ_PER_MHZ 1000
#define MHZ_PER_GHZ 1000.0
#define BYTES_PER_KB 1024ULL
#define BYTES_PER_MB (BYTES_PER_KB * BYTES_PER_KB)
#define BYTES_PER_GB (BYTES_PER_MB * BYTES_PER_KB)
#define BYTES_PER_KB_F 1024.0
#define BYTES_PER_MB_F (BYTES_PER_KB_F * BYTES_PER_KB_F)
#define BYTES_PER_GB_F (BYTES_PER_MB_F * BYTES_PER_KB_F)
#define PERCENT_BASE 100
#define TEMP_MILLI_DIV 1000
#define DISPLAY_UNIT_THRESHOLD 1536
#define ONCE_WAIT_SEC 3
#define RETRY_MAX 3
#define BAT_NUM_SAMPLES 10

#define PATH_SZ 256
#define PATH_BUF_SZ 128
#define BIG_BUF 2048
#define DESC_LEN 64
#define RATE_SZ 16
#define CUP_BUF_SZ 32
#define IP4_SZ 16
#define IP6_SZ 48
#define WTHR_TEMP_SZ 5
#define WTHR_CODE_SZ 6
#define GOVERNOR_SZ 20
#define NAME_SZ 32

int is_virtual_kind(const char *kind);

struct fentry {
  char name[CUP_BUF_SZ];
  int val;
};
#define CARD_PATH_SZ 80
#define SSID_SZ 33

#define CPU_TEMP_PATH_SZ 160
#define FSTYPE_SZ 32
#define RTNL_REQ_SZ 48

#define IFLA_KIND_MAX 32

enum mon_action_type {
  MON_NONE = 0,
  MON_LINK_ADD,
  MON_LINK_REMOVE,
  MON_ADDR4_ADD,
  MON_ADDR4_REMOVE,
  MON_ADDR6_ADD,
  MON_ADDR6_REMOVE,
  MON_ROUTE_ADD,
  MON_ROUTE_REMOVE,
};

struct mon_action_link {
  int ifindex;
  char name[IFACE_NAME_LEN];
  char kind[IFLA_KIND_MAX];
  unsigned int flags;
};

union mon_action_arg {
  struct mon_action_link link;
  struct {
    int ifindex;
    unsigned char prefixlen;
    union {
      struct in_addr v4;
      struct in6_addr v6;
    } addr;
    int family;
  } addr;
  struct {
    int ifindex;
    unsigned char table;
    unsigned char dst_len;
    int family;
  } route;
};

struct mon_action {
  enum mon_action_type type;
  union mon_action_arg arg;
};

struct mon_action_fifo {
  struct mon_action *items;
  size_t count;
  size_t cap;
  pthread_mutex_t lock;
};
#define GPU_FMT_SZ 160
#define POLL_WAIT_MS 100
#include "ioserv.h"

int read_file(const char *path, char *b, size_t sz);
int read_uint(const char *path, unsigned long long *val);

static inline int fmt_freq(char *buf, int sz, int cur, int max)
{
  if (max) {
    if (max > DISPLAY_UNIT_THRESHOLD)
      return snprintf(buf, sz, "%.1f/%.1fGHz", cur / MHZ_PER_GHZ, max / MHZ_PER_GHZ);
    return snprintf(buf, sz, "%d/%dMHz", cur, max);
  }
  if (cur > DISPLAY_UNIT_THRESHOLD)
    return snprintf(buf, sz, "%.1fGHz", cur / MHZ_PER_GHZ);
  if (cur)
    return snprintf(buf, sz, "%dMHz", cur);
  return 0;
}

static inline int temp_from_milli(const char *path)
{
  unsigned long long v;
  return read_uint(path, &v) == 0 ? (int)(v / TEMP_MILLI_DIV) : -1;
}

char *slurp(const char *path);
struct clock_state;
struct cpu_keep;
int cpu_info_pct(struct clock_state *ci);
int cpu_temp_c(const struct cpu_keep *keep);
void get_cpu_freqs(struct clock_state *ci);
void get_cpu_extra(struct clock_state *ci);
void cpu_discover(struct cpu_keep *k);
void mem_usage(struct clock_state *ci);
void get_container_mem(struct clock_state *ci);
void mem_discover(struct cpu_keep *k);
void get_battery(struct clock_state *ci);
void bat_discover(struct cpu_keep *k);
void bat_estimate(struct clock_state *ci, int cur_raw, int state, time_t now);
void get_uptime(struct clock_state *ci);

struct netlink_ctx {
  int fd;
  unsigned seq;
  int family;
  int ifindex;
};

/* Generic netlink infrastructure */
struct nlmsghdr;
struct genlmsghdr;
#define NLBUF 4096
typedef int (*nl_dump_cb)(const struct nlmsghdr *, void *);
typedef int (*rtnl_cb)(const struct nlmsghdr *, void *);
struct msghdr;
ssize_t nlk_recv(int fd, struct msghdr *msg, int flags);
int talk(const struct netlink_ctx *nlk, void *buf);
int talk_dump(const struct netlink_ctx *nlk, void *buf);
void init_req(struct nlmsghdr *nh, struct genlmsghdr *gh, struct netlink_ctx *nlk, int type, int cmd);
int nl_dump_iter(struct netlink_ctx *nlk, int cmd, nl_dump_cb cb, void *arg);

int nlk_init(struct netlink_ctx *nlk);
struct wlan_cache;
int nlk_find_wlan_all(struct netlink_ctx *nlk, struct wlan_cache *cache);
struct net_ctx;
int nlk_station_rate(struct netlink_ctx *nlk, const char *iface, struct net_ctx *net);
int nlk_scan_bss(struct netlink_ctx *nlk, const char *iface, struct net_ctx *net);

struct link_entry {
  int ifindex;
  char name[IFACE_NAME_LEN];
  int is_virtual;
  int speed;                    /* Mb/s, -1 = not yet fetched */
  unsigned long long rx;
  unsigned long long tx;
};

#define NAME_IDX_CAP 8
struct name_idx {
  char name[IFACE_NAME_LEN];
  unsigned ifindex;
};

struct rtnl_ctx {
  int fd;
  unsigned seq;
  char v6_cache[IFACE_NAME_LEN];
  int v6_cached;
  struct link_entry *links;
  int link_count;
  int link_cap;
  int links_stale;
  int refreshed;
  struct name_idx *name_idx;
  int name_idx_n;
  int name_idx_cap;
};

struct rtnl_ctx;
struct nlmsghdr;
struct net_ctx;

int rtnl_open(struct rtnl_ctx *r);
int rtnl_dump(struct rtnl_ctx *r, int type, int family, rtnl_cb cb, void *arg);
int rtnl_route_dump(struct rtnl_ctx *r, int family, rtnl_cb cb, void *arg);
int nlk_recv_one(const struct rtnl_ctx *r, unsigned char *buf, size_t sz, ssize_t * np);
int nl_drain_done(int fd);
int nl_send_dump(int fd, unsigned char *buf, size_t len);
int nl_handle_buf(unsigned char *buf, size_t len, rtnl_cb cb, void *arg);
struct gw_track {
  int *idx;
  int *metric;
  int cap;
  int n;
};

int rtnl_default_v6(struct rtnl_ctx *r, char *out, int sz);
int rtnl_default_v4_idx(struct rtnl_ctx *r, char *out, int sz, struct gw_track *gw);
int rtnl_default_v6_idx(struct rtnl_ctx *r, char *out, int sz, struct gw_track *gw);
void rtnl_cache_links(struct rtnl_ctx *r);
void rtnl_read_dev(struct rtnl_ctx *r, struct net_ctx *c, unsigned long long *rd, unsigned long long *td);
int rtnl_find_addr6(struct rtnl_ctx *r, const char *target, char *ip6, size_t sz);
const char *name_idx_by_idx(struct rtnl_ctx *r, unsigned ifindex);
unsigned name_idx_by_name(struct rtnl_ctx *r, const char *name);
struct rtnl_mon_ctx {
  int fd;
  int addr4_changed;
  int addr6_changed;
  volatile int stop;
  struct mon_action_fifo fifo;
  pthread_t thread;
  int started;
};

int rtnl_open_monitor(struct rtnl_mon_ctx *m);
int rtnl_monitor_start(struct rtnl_mon_ctx *m);
void rtnl_monitor_stop(struct rtnl_mon_ctx *m);
void push_link_action(struct rtnl_mon_ctx *m, const struct nlmsghdr *nh, enum mon_action_type type);
void push_addr_action(struct rtnl_mon_ctx *m, const struct nlmsghdr *nh, enum mon_action_type type);
void push_route_action(struct rtnl_mon_ctx *m, const struct nlmsghdr *nh, enum mon_action_type type);

struct fetch_retry {
  unsigned char try;
  unsigned long long retry_ts;
};

struct dev_out {
  char name[NAME_SZ];
  unsigned major, minor;
  int is_virtual;
  int has_temp;
  unsigned char temp_path[DESC_LEN];
  unsigned long long rp, wp;
  unsigned long long size;
};

struct mount_ctx;

struct disk_ctx {
  struct dev_out *devs;
  unsigned cnt;
  unsigned cap;
  struct mount_ctx *mnt;
};

struct gpu_ctx {
  unsigned int present;
  char card_path[CARD_PATH_SZ];
  char hwmon_path[CARD_PATH_SZ];
  char card_token[CARD_PATH_SZ];
  char temp_path[GPU_FMT_SZ];
  char fan_path[GPU_FMT_SZ];
  unsigned long long rc6_prev;
  unsigned long long ts_sec;
  unsigned long long ts_nsec;
  int no_card;
};

#define GPU_HAS_BUSY_PCT  (1u << 0)
#define GPU_HAS_PP_SCLK   (1u << 1)
#define GPU_HAS_PP_MCLK   (1u << 2)
#define GPU_HAS_MEM       (1u << 3)
#define GPU_HAS_TEMP      (1u << 4)
#define GPU_HAS_FAN       (1u << 5)
#define GPU_HAS_GOV       (1u << 6)
#define GPU_HAS_GT_FREQ   (1u << 7)
#define GPU_HAS_RC6       (1u << 8)

struct clock_state;
void sto_read_throughput(struct clock_state *ci);
struct net_ctx;
void pick_primary(struct net_ctx *c, struct rtnl_ctx *r, int use_cached);
void find_wlan_nlk(struct clock_state *ci);
void get_local_ip6(struct clock_state *ci);
void refresh_local_ips(struct clock_state *ci);

int get_dgram_fd(struct net_ctx *n);
void read_net_dev(struct clock_state *ci, struct rtnl_ctx *r);
void do_wlan(struct clock_state *ci);
void get_net_info(struct clock_state *ci);
void wan_dns_start(struct clock_state *ci, int v6);
int parse_body(const char *buf, char *ip, size_t sz);
int weather_parse_resp(const char *resp, char *out, size_t outsz);
enum dns_state { DNS_IDLE, DNS_RUNNING, DNS_DONE, DNS_ERROR = -1 };
enum wan_state { WAN_IDLE, WAN_DNS };

struct dns_slot {
  pthread_mutex_t lock;
  struct sockaddr_storage addr;
  socklen_t addrlen;
  enum dns_state state;
  pthread_t thread;
  int cancelled;
};

struct async_ctx;
struct dns_arg {
  struct dns_slot *slot;
  const char *host;
  int family;
  struct async_ctx *ctx;
  struct addrinfo *ai;
};

int dns_read_slot(struct dns_slot *slot, struct sockaddr_storage *ss, socklen_t salen);
void dns_cancel(struct dns_slot *slot);
void dns_start(struct async_ctx *ctx, struct dns_slot *slot, const char *host, int family);
struct dns_slot *wan_dns_slot(struct async_ctx *ctx, int v6);
void weather_dns_start(struct clock_state *ci);
void weather_cleanup(struct clock_state *ci);
void restore_weather(struct clock_state *ci);
void try_refetch_weather(struct clock_state *ci, unsigned long long now);
void pump_weather_result(struct clock_state *ci, unsigned long long now);
int any_ready(unsigned char t, unsigned long long retry, unsigned long long now);
int stage_ready(const struct fetch_retry *fr, unsigned long long now);
void fetch_ok_reset(struct fetch_retry *fr);
void fetch_err(struct fetch_retry *fr, unsigned long long now);
void close_conn_fd(int *fd);
void poll_async_fetches(struct clock_state *ci, time_t now);
void once_wait(struct clock_state *ci, unsigned long long t0);
void get_gpu_info(struct clock_state *ci);
void gpu_collect(struct clock_state *ci);
void gpu_probe(struct gpu_ctx *g);
void get_fan_info(struct clock_state *ci);
void fan_discover(struct cpu_keep *k);
const char *weather_emoji(int code);

#define MAX_NET 3

struct wlan_cache {
  int *ifindices;
  char (*ssids)[SSID_SZ];
  int count;
  int cap;
  int fresh;
};

struct net_ctx {
  int ifindex[MAX_NET];
  unsigned count;
  int has_phys;
  int needs_reprimary;
  int wlan_idx;
  int gw_idx[MAX_NET];
  int gw_metric[MAX_NET];
  int gw_n;
  unsigned long long rp[MAX_NET], tp[MAX_NET];
  int link_mbps[MAX_NET];
  int wlan_dbm;
  int wlan_rx_rate;
  int wlan_tx_rate;
  char wlan_ssid[SSID_SZ];
  int link_idx[MAX_NET];
  int needs_route;
  int dgram_fd;
  struct wlan_cache wcache;
  int wcache_stale;
  unsigned char bss_mac[6];
};
#endif
