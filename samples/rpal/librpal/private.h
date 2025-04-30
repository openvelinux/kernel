#ifndef PRIVATE_H
#define PRIVATE_H

#include <unistd.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#ifdef __x86_64__
#include <immintrin.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <fcntl.h>
#include <stddef.h>

#include "debug.h"
#include "rpal_queue.h"
#include "fiber.h"
#include "rpal.h"

#ifdef __x86_64__
static inline void write_tls_base(unsigned long tls_base)
{
	asm volatile("wrfsbase %0" ::"r"(tls_base) : "memory");
}

static inline unsigned long read_tls_base(void)
{
	unsigned long fsbase;
	asm volatile("rdfsbase %0" : "=r"(fsbase)::"memory");
	return fsbase;
}
#endif

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

// | fd_timestamp |   pad   | rthread_id | server_fd |
// |     16       |    8    |      8     |    32     |
#define LOW32_MASK ((1UL << 32) - 1)
#define MIDL8_MASK ((unsigned long)(((1UL << 8) - 1)) << 32)

#define HIGH16_OFFSET 48
#define HIGH32_OFFSET 32

#define get_high16(val) ({ (val) >> HIGH16_OFFSET; })

#define get_high32(val) ({ (val) >> HIGH32_OFFSET; })

#define get_midl8(val) ({ ((val) & MIDL8_MASK) >> HIGH32_OFFSET; })
#define get_low32(val) ({ (val) & LOW32_MASK; })

#define get_fdtimestamp(rpalfd) get_high16(rpalfd)
#define get_rid(rpalfd) get_midl8(rpalfd)
#define get_sfd(rpalfd) get_low32(rpalfd)

#define PAGE_SIZE 4096
#define DEFUALT_STACK_SIZE (PAGE_SIZE * 4)
#define TRAMPOLINE_SIZE (PAGE_SIZE * 1)

#define BITS_PER_LONG 64
#define BITS_TO_LONGS(x)                                                       \
	(((x) + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long)))

#define KEY_SIZE 16

enum rpal_task_status {
	RPAL_TASK_DONE,
	RPAL_TASK_BLOCKED,
	RPAL_TASK_KERNEL_RET,
};

enum rpal_epoll_event {
	RPAL_KERNEL_PENDING = 0x1,
	RPAL_USER_PENDING = 0x2,
};

enum rpal_epoll_status {
	RPAL_EP_SYS,
	RPAL_EP_KSYS,
	RPAL_EP_WAIT,
	RPAL_EP_APP,
	RPAL_EP_KAPP,
	RPAL_EP_READY_WAIT,
	RPAL_EP_READY_WAIT_LS,
	RPAL_EP_ERR,
	RPAL_EP_MAX,
};

enum rpal_command_type {
	RPAL_CMD_GET_API_VERSION_AND_CAP,
	RPAL_CMD_GET_SERVICE_KEY,
	RPAL_CMD_REQUEST_SERVICE,
	RPAL_CMD_RELEASE_SERVICE,
	RPAL_CMD_ENABLE_SERVICE,
	RPAL_CMD_DISABLE_SERVICE,
	RPAL_CMD_REGISTER_THREAD,
	RPAL_CMD_UDS_FDMAP,
	RPAL_CMD_GET_SERVICE_ID,
	RPAL_CMD_GET_SERVICE_PKEY,
	RPAL_NR_CMD,
};

enum {
	RPAL_REGISTER_SENDER_THREAD,
	RPAL_REGISTER_RECEIVER_THREAD,
	RPAL_UNREGISTER_SENDER_THREAD,
	RPAL_UNREGISTER_RECEIVER_THREAD,
};

typedef enum rpal_thread_status {
	RPAL_THREAD_UNINITIALIZED,
	RPAL_THREAD_INITIALIZED,
	RPAL_THREAD_AVAILABLE,
} rpal_thread_status_t;

enum RPAL_CAPABILITIES {
	RPAL_CAP_PKU,
};

#define RPAL_EP_SID_SHIFT 24
#define RPAL_EP_ID_SHIFT 8
#define RPAL_EP_STATUS_MASK ((1UL << RPAL_EP_ID_SHIFT) - 1)
#define RPAL_EP_SID_MASK (~((1UL << RPAL_EP_SID_SHIFT) - 1))
#define RPAL_EP_ID_MASK (~(0UL | RPAL_EP_STATUS_MASK | RPAL_EP_SID_MASK))
#define RPAL_EP_MAX_ID ((1 << (RPAL_EP_SID_SHIFT - RPAL_EP_ID_SHIFT)) - 1)
#define RPAL_BUILD_EP_APP(id, sid)                                             \
	((sid << RPAL_EP_SID_SHIFT) | (id << RPAL_EP_ID_SHIFT) | RPAL_EP_APP)

typedef struct rpal_capability {
	int compat_version;
	int api_version;
	unsigned long cap;
} rpal_capability_t;

typedef struct ksave_context {
	unsigned long r15;
	unsigned long r14;
	unsigned long r13;
	unsigned long r12;
	unsigned long rbx;
	unsigned long rbp;
	unsigned long rip;
	unsigned long rsp;
} ksave_context_t;

typedef struct rpal_epoll_context {
	ksave_context_t kcontext;
	int epfd;
	void *ep_events;
	int ep_maxevents;
	int ep_timeout;
	int rid; // id in rpal_thread_pool
	int rpal_ep_poll;
	int ep_status;
	int ep_pending;
	int g_status;
	uint32_t pkru;
	int64_t total_time;
	int64_t start_time;
	int64_t end_time;
} rpal_epoll_context_t;

typedef struct rpal_call_info {
	unsigned long app_tls_base;
	uint32_t pkru;
	fcontext_t app_fctx;
} rpal_call_info_t;

typedef struct rpal_service_metadata rpal_thread_pool_t;

enum thread_type {
	RPAL_THREAD = 0x1,
	RPAL_SENDER = 0x2,
};
typedef struct rpal_thread_info {
	long tid;
	unsigned long ser_tls_base;

	int epfd;
	rpal_thread_status_t status;
	epoll_uevent_queue_t ueventq;
	volatile uint64_t uqlock;

	fcontext_t main_ctx;
	task_t *ep_stack;
	task_t *trampoline;

	rpal_call_info_t rci;

	volatile rpal_epoll_context_t *async_epctx;
	rpal_thread_pool_t *rtp;
} rpal_thread_info_t;

typedef struct fd_table fd_table_t;
/* Keep it the same as kernel */
struct rpal_service_metadata {
	rpal_thread_info_t *rtis;
	fd_table_t *fdt;
	uint64_t service_key;
	int nr_threads;
	int service_id;
	int pkey;
};

#define RPAL_ERROR_MAGIC 0x98CC98CC

typedef struct rpal_error_context {
	unsigned long tls_base;
	uint64_t erip;
	uint64_t ersp;
	int state;
} rpal_error_context_t;

typedef struct rpal_app_context {
	ksave_context_t kcontext;
	int sender_id;
	int magic;
	rpal_error_context_t ec;
	int64_t start_time;
	int64_t total_time;
} rpal_app_context_t;

#define RPAL_EP_POLL_MAGIC 0xCC98CC98

typedef struct rpal_sender_info {
	int idx;
	int tid;
	int pkey;
	int inited;
	rpal_app_context_t async_stk;
} rpal_sender_info_t;

typedef struct fdt_node fdt_node_t;

typedef struct fd_event {
	int epfd;
	int fd;
	struct epoll_event epev;
	uint32_t events;
	int wait;

	rpal_queue_t q;
	int pkey; // unused
	fdt_node_t *node;
	struct fd_event *next;
	uint16_t timestamp;
	uint16_t outdated;
	uint64_t service_key;
} fd_event_t;

struct fdt_node {
	fd_event_t **events;
	fdt_node_t *next;
	int *ref_count;
	uint16_t *timestamps;
};

// when sender calls fd_event_get, we must check this number to avoid
// accessing outdated fdt_node definitions

#define FDTAB_MAG1 0x4D414731UL // add fde lazyswitch
#define FDTAB_MAG2 0x14D414731UL // add fde timestamp
#define FDTAB_MAG3 0x34D414731UL // add fde outdated
#define FDTAB_MAG4 0x74D414731UL // add automatic identification rpal mode

enum fde_ref_status {
	FDE_FREEING = -100,
	FDE_FREED = -1,
	FDE_AVAILABLE = 0,
};

#define DEFAULT_NODE_SHIFT 14 // 2^14 elements per node
typedef struct fd_table {
	fdt_node_t *head;
	fdt_node_t *tail;
	int max_fd;
	unsigned int node_shift;
	unsigned int node_mask;
	pthread_mutex_t lock;
	unsigned long magic;
	fd_event_t *freelist;
	pthread_mutex_t list_lock;
} fd_table_t;

typedef struct rpal_critical_section {
	unsigned long ret_begin;
	unsigned long ret_end;
	unsigned long call_begin;
	unsigned long call_end;
} rpal_critical_section_t;

#ifndef RPAL_DEBUG
#define dbprint(category, format, args...) ((void)0)
#else
void dbprint(rpal_debug_flag_t category, char *format, ...)
	__attribute__((format(printf, 2, 3)));
#endif
void errprint(const char *format, ...) __attribute__((format(printf, 1, 2)));
void warnprint(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
