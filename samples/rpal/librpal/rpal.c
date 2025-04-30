#include "private.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <linux/futex.h>
#include <signal.h>
#include <stdarg.h>

#include "rpal_pkru.h"

/* prints an error message to stderr */
void errprint(const char *format, ...)
{
	va_list args;

	fprintf(stderr, "[RPAL_ERROR] ");
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

/* prints a warning message to stderr */
void warnprint(const char *format, ...)
{
	va_list args;

	fprintf(stderr, "[RPAL_WARNING] ");
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

#ifdef RPAL_DEBUG
void dbprint(rpal_debug_flag_t category, char *format, ...)
{
	if (category & RPAL_DEBUG) {
		va_list args;
		fprintf(stderr, "[RPAL_DEBUG] ");
		va_start(args, format);
		vfprintf(stderr, format, args);
		va_end(args);
	}
}
#endif

#ifdef RPAL_STATS
#define DEFINE_COUNT(name) static uint32_t rpal_count_##name

#define DEFINE_COUNT_ARRAY(name, size) static uint32_t rpal_count_##name[size]

#define RPAL_COUNT_INC(name)                                                   \
	__atomic_fetch_add(&rpal_count_##name, 1, __ATOMIC_RELAXED)

#define RPAL_COUNT_ADD(name, add)                                              \
	__atomic_fetch_add(&rpal_count_##name, add, __ATOMIC_RELAXED)

#define RPAL_COUNT_ARRAY_INC(name, idx)                                        \
	__atomic_fetch_add(&rpal_count_##name[idx], 1, __ATOMIC_RELAXED)

#define RPAL_COUNT_GET(name) (rpal_count_##name)

#define RPAL_COUNT_ARRAY_GET(name, idx) (rpal_count_##name[idx])

#define RPAL_COUNT_PRINT(name)                                                 \
	printf("%-26s = %10x\n", #name, rpal_count_##name)

#define RPAL_COUNT_ARRAY_PRINT(name, idx, idx_string)                          \
	printf("%-10s[%-10s] = %10x\n", #name, idx_string[idx],                \
	       rpal_count_##name[idx])

DEFINE_COUNT(rpalin_events);
DEFINE_COUNT(rpalout_events);
DEFINE_COUNT(rpal_lazywake);
DEFINE_COUNT(rpalcall_jmp);
DEFINE_COUNT(rpalcall_pending);
DEFINE_COUNT(rpalcall_ret);
DEFINE_COUNT(sender_kernel_ret);
DEFINE_COUNT(may_loss);
DEFINE_COUNT_ARRAY(pending_reason, RPAL_EP_MAX);
static char *ep_status_s[RPAL_EP_MAX] = {
	"EP_SYS",  "EP_KSYS", "EP_WAIT",    "EP_APP",
	"EP_KAPP", "EP_RWIT", "EP_RWIT_LS", "EP_ERR",
};
#else
#define DEFINE_COUNT(name)
#define DEFINE_COUNT_ARRAY(name, size)
#define RPAL_COUNT_INC(name)
#define RPAL_COUNT_ADD(name, add)
#define RPAL_COUNT_ARRAY_INC(name, idx)
#define RPAL_COUNT_GET(name)
#define RPAL_COUNT_ARRAY_GET(name, idx)
#endif

#define SAVE_FPU(mxcsr, fpucw)                                                 \
	__asm__ __volatile__("stmxcsr %0;"                                     \
			     "fnstcw %1;"                                      \
			     : "=m"(mxcsr), "=m"(fpucw)                        \
			     :)
#define RESTORE_FPU(mxcsr, fpucw)                                              \
	__asm__ __volatile__("ldmxcsr %0;"                                     \
			     "fldcw %1;"                                       \
			     :                                                 \
			     : "m"(mxcsr), "m"(fpucw))

#define ERRREPORT(EPTR, ECODE, ...)                                            \
	if (EPTR) {                                                            \
		*EPTR = ECODE;                                                 \
	}                                                                      \
	errprint(__VA_ARGS__);

#define RPAL_MGT_FILE "/proc/rpal"
#define RPAL_SWITCH_FILE "/proc/rpal_enabled"
#define RPAL_ENABLED 1
#define MAX_SUPPROTED_CPUS 192

static __always_inline unsigned long __ffs(unsigned long word)
{
	asm("rep; bsf %1,%0" : "=r"(word) : "rm"(word));

	return word;
}

static void __set_bit(uint64_t *bitmap, int idx)
{
	int bit, i;
	i = idx / 8;
	bit = idx % 8;
	bitmap[i] |= (1UL << bit);
}

static int clear_first_set_bit(uint64_t *bitmap, int size)
{
	int idx;
	int bit, i;

	for (i = 0; i * BITS_PER_LONG < size; i++) {
		if (bitmap[i]) {
			bit = __ffs(bitmap[i]);
			idx = i * BITS_PER_LONG + bit;
			if (idx >= size) {
				return -1;
			}
			bitmap[i] &= ~(1UL << bit);
			return idx;
		}
	}
	return -1;
}

extern void rpal_get_critical_addr(rpal_critical_section_t *rcs);
static rpal_critical_section_t rcs = { 0 };

#define MAX_SERVICEID 254 // Intel MPK Limit
#define MIN_RPAL_KERNEL_API_VERSION 1
#define TARGET_RPAL_KERNEL_API_VERSION                                         \
	1 // RPAL will disable when KERNEL_API < TARGET_RPAL_KERNEL_API_VERSION

enum {
	RCALL_IN = 0x1 << 0,
	RCALL_OUT = 0x1 << 1,
};

enum {
	FDE_NO_TRIGGER,
	FDE_TRIGGER_OUT,
};

#define EPOLLRPALINOUT_BITS (EPOLLRPALIN | EPOLLRPALOUT)

#define DEFAULT_QUEUE_SIZE 32U

typedef struct rpal_requested_service {
	rpal_thread_pool_t *service;
} rpal_requeseted_service_t;

static int rpal_mgtfd = -1;
static int rpal_switchfd = -1;
static char *rpal_enabled_ptr;
static char *rpal_enabled;
static int inited;
int pkru_enabled = 0;

static rpal_capability_t version;
static pthread_key_t rpal_key;
static rpal_requeseted_service_t requested_services[MAX_SERVICEID];
static pthread_mutex_t release_lock;

typedef struct rpal_local {
	unsigned int tflag;
	rpal_thread_info_t *rti;
	rpal_sender_info_t *rsi;
} rpal_local_t;

#define SENDERS_PAGE_ORDER 3
#define RPALTHREAD_PAGE_ORDER 0

typedef struct rpal_thread_metadata {
	int rpal_thread_idx;
	int service_id;
	const int epcpage_order;
	uint64_t service_key;
	rpal_thread_pool_t *recver_rtp;
	rpal_epoll_context_t *ep_contexts;
	pid_t pid;
	int *eventfds;
} rpal_thread_metadata_t;

static rpal_thread_metadata_t threads_md = {
	.service_id = -1,
	.epcpage_order = RPALTHREAD_PAGE_ORDER,
};

static inline rpal_sender_info_t *current_rpal_sender(void)
{
	rpal_local_t *local;

	local = pthread_getspecific(rpal_key);
	if (local && (local->tflag & RPAL_SENDER)) {
		return local->rsi;
	} else {
		return NULL;
	}
}

static inline rpal_thread_info_t *current_rpal_thread(void)
{
	rpal_local_t *local;

	local = pthread_getspecific(rpal_key);
	if (local && (local->tflag & RPAL_THREAD)) {
		return local->rti;
	} else {
		return NULL;
	}
}

static status_t rpal_register_sender_local(rpal_sender_info_t *sender)
{
	rpal_local_t *local;
	local = pthread_getspecific(rpal_key);
	if (!local) {
		local = malloc(sizeof(rpal_local_t));
		if (!local)
			return RPAL_FAILURE;
		memset(local, 0, sizeof(rpal_local_t));
		pthread_setspecific(rpal_key, local);
	}
	if (local->tflag & RPAL_SENDER) {
		return RPAL_FAILURE;
	}
	local->rsi = sender;
	local->tflag |= RPAL_SENDER;
	return RPAL_SUCCESS;
}

static status_t rpal_unregister_sender_local(void)
{
	rpal_local_t *local;
	local = pthread_getspecific(rpal_key);
	if (!local || !(local->tflag & RPAL_SENDER))
		return RPAL_FAILURE;

	local->rsi = NULL;
	local->tflag &= ~RPAL_SENDER;
	if (!local->tflag) {
		pthread_setspecific(rpal_key, NULL);
		free(local);
	}
	return RPAL_SUCCESS;
}

static status_t rpal_register_receiver_local(rpal_thread_info_t *thread)
{
	rpal_local_t *local;
	local = pthread_getspecific(rpal_key);
	if (!local) {
		local = malloc(sizeof(rpal_local_t));
		if (!local)
			return RPAL_FAILURE;
		memset(local, 0, sizeof(rpal_local_t));
		pthread_setspecific(rpal_key, local);
	}
	if (local->tflag & RPAL_THREAD) {
		return RPAL_FAILURE;
	}
	local->rti = thread;
	local->tflag |= RPAL_THREAD;
	return RPAL_SUCCESS;
}

static status_t rpal_unregister_receiver_local(void)
{
	rpal_local_t *local;
	local = pthread_getspecific(rpal_key);
	if (!local || !(local->tflag & RPAL_THREAD))
		return RPAL_FAILURE;

	local->rti = NULL;
	local->tflag &= ~RPAL_THREAD;
	if (!local->tflag) {
		pthread_setspecific(rpal_key, NULL);
		free(local);
	}
	return RPAL_SUCCESS;
}

#define MAX_SENDERS 256
typedef struct rpal_senders_metadata {
	uint64_t bitmap[BITS_TO_LONGS(MAX_SENDERS)];
	pthread_mutex_t lock;
	int sdpage_order;
	rpal_sender_info_t *senders;
} rpal_senders_metadata_t;

static rpal_senders_metadata_t *senders_md;

static long rpal_ctl(unsigned long cmd, unsigned long arg0, unsigned long arg1)
{
	struct {
		unsigned long *ret;
		unsigned long cmd;
		unsigned long arg0;
		unsigned long arg1;
	} args;
	const int args_size = sizeof(args);
	int ret;

	if (rpal_mgtfd == -1) {
		errprint("rpal_mgtfd is not opened\n");
		return -1;
	}

	args.ret = (unsigned long *)(&(args.ret));
	args.cmd = cmd;
	args.arg0 = arg0;
	args.arg1 = arg1;

	ret = write(rpal_mgtfd, &args, args_size);
	if (ret != args_size) {
		errprint("rpal_write error\n");
		return -1;
	}

	return (long)args.ret;
}

static inline long rpal_register_sender(rpal_sender_info_t *sender)
{
	long ret;

	if (rpal_register_sender_local(sender) == RPAL_FAILURE)
		return RPAL_FAILURE;

	ret = rpal_ctl(RPAL_CMD_REGISTER_THREAD,
		       (unsigned long)&sender->async_stk,
		       RPAL_REGISTER_SENDER_THREAD);
	if (ret < 0) {
		rpal_unregister_sender_local();
	}
	return ret;
}

static inline long rpal_register_receiver(rpal_thread_info_t *rti)
{
	long ret;

	if (rpal_register_receiver_local(rti) == RPAL_FAILURE)
		return RPAL_FAILURE;
	ret = rpal_ctl(RPAL_CMD_REGISTER_THREAD,
		       (unsigned long)rti->async_epctx,
		       RPAL_REGISTER_RECEIVER_THREAD);
	if (ret < 0) {
		rpal_unregister_receiver_local();
	}
	return ret;
}

static inline long rpal_unregister_sender(void)
{
	if (rpal_unregister_sender_local() == RPAL_FAILURE)
		return RPAL_FAILURE;
	return rpal_ctl(RPAL_CMD_REGISTER_THREAD, 0,
			RPAL_UNREGISTER_SENDER_THREAD);
}

static inline long rpal_unregister_receiver(void)
{
	if (rpal_unregister_receiver_local() == RPAL_FAILURE)
		return RPAL_FAILURE;
	return rpal_ctl(RPAL_CMD_REGISTER_THREAD, 0,
			RPAL_UNREGISTER_RECEIVER_THREAD);
}

static int rpal_get_service_pkey(void)
{
	int pkey;

	pkey = (int)rpal_ctl(RPAL_CMD_GET_SERVICE_PKEY, 0, 0);
	if (pkey == -1) {
		warnprint("MPK not supported on this host, disabling PKRU\n");
		return -1;
	}
	return pkey;
}

static int __rpal_get_service_id(void)
{
	return (int)rpal_ctl(RPAL_CMD_GET_SERVICE_ID, 0, 0);
}

static uint64_t __rpal_get_service_key(void)
{
	return rpal_ctl(RPAL_CMD_GET_SERVICE_KEY, 0, 0);
}

static void *rpal_get_shared_page(int order)
{
	void *p;
	int size;
	int flags = MAP_SHARED;

	if (rpal_mgtfd == -1) {
		return NULL;
	}
	size = PAGE_SIZE * (1 << order);

	p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, rpal_mgtfd, 0);

	return p;
}

static int rpal_free_shared_page(void *page, int order)
{
	int ret = 0;
	int size;

	size = PAGE_SIZE * (1 << order);
	ret = munmap(page, size);
	if (ret) {
		errprint("munmap fail: %d\n", ret);
	}
	return ret;
}

static inline int rpal_inited(void)
{
	return (inited == 1);
}

static inline int sender_idx_is_invalid(int idx)
{
	if (idx < 0 || idx >= MAX_SENDERS)
		return 1;
	return 0;
}

static int rpal_sender_info_alloc(rpal_sender_info_t **sender)
{
	int idx;

	if (!senders_md)
		return RPAL_FAILURE;
	pthread_mutex_lock(&senders_md->lock);
	idx = clear_first_set_bit(senders_md->bitmap, MAX_SENDERS);
	if (idx < 0) {
		errprint("sender data alloc failed: %d, bitmap: %lx\n", idx,
			 senders_md->bitmap[0]);
		goto unlock;
	}
	*sender = senders_md->senders + idx;

unlock:
	pthread_mutex_unlock(&senders_md->lock);
	return idx;
}

static void rpal_sender_info_free(int idx)
{
	if (sender_idx_is_invalid(idx)) {
		return;
	}
	pthread_mutex_lock(&senders_md->lock);
	__set_bit(senders_md->bitmap, idx);
	pthread_mutex_unlock(&senders_md->lock);
}

extern unsigned long rpal_get_ret_rip(void);

static int rpal_sender_inited(rpal_sender_info_t *sender)
{
	return (sender->inited == 1);
}

status_t rpal_sender_init(rpal_error_code_t *error)
{
	int idx;
	int ret = RPAL_FAILURE;
	rpal_sender_info_t *sender;

	if (!rpal_inited()) {
		ERRREPORT(error, RPAL_DONT_INITED, "%s: rpal do not init\n",
			  __FUNCTION__);
		goto error_out;
	}
	sender = current_rpal_sender();
	if (sender) {
		goto error_out;
	}
	idx = rpal_sender_info_alloc(&sender);
	if (idx < 0) {
		if (error) {
			*error = RPAL_ERR_SENDER_INIT;
		}
		goto error_out;
	}
	sender->idx = idx;
	sender->async_stk.sender_id = idx;
	sender->tid = syscall(SYS_gettid);
	sender->pkey = rpal_get_service_pkey();
	sender->async_stk.ec.erip = rpal_get_ret_rip();
	ret = rpal_register_sender(sender);
	if (ret) {
		ERRREPORT(error, RPAL_ERR_SENDER_REG,
			  "rpal_register_sender error: %d\n", ret);
		goto sender_register_failed;
	}
	sender->inited = 1;
	return RPAL_SUCCESS;

sender_register_failed:
	rpal_sender_info_free(idx);
error_out:
	return RPAL_FAILURE;
}

status_t rpal_sender_exit(void)
{
	int idx;
	rpal_sender_info_t *sender;

	sender = current_rpal_sender();

	if (sender) {
		idx = sender->idx;
		sender->idx = 0;
		sender->tid = 0;
		rpal_unregister_sender();
		rpal_sender_info_free(idx);
		sender->pkey = 0;
	}
	return RPAL_SUCCESS;
}

static status_t rpal_enable_service(rpal_error_code_t *error)
{
	long ret = 0;

	ret = rpal_ctl(RPAL_CMD_ENABLE_SERVICE,
		       (unsigned long)threads_md.recver_rtp,
		       (unsigned long)&rcs);
	if (ret) {
		ERRREPORT(error, RPAL_ERR_ENABLE_SERVICE,
			  "rpal enable service failed: %ld\n", ret)
		return RPAL_FAILURE;
	}
	return RPAL_SUCCESS;
}

static status_t rpal_disable_service(void)
{
	long ret = 0;
	ret = rpal_ctl(RPAL_CMD_DISABLE_SERVICE, 0, 0);
	if (ret) {
		errprint("rpal disable service failed: %ld\n", ret);
		return RPAL_FAILURE;
	}
	return RPAL_SUCCESS;
}

static status_t add_requested_service(rpal_thread_pool_t *rtp)
{
	int id;
	rpal_thread_pool_t *expected = NULL;

	if (!rtp) {
		errprint("add requested service null\n");
		return RPAL_FAILURE;
	}
	id = rtp->service_id;
	if (id >= MAX_SERVICEID) {
		errprint("add requested service %d error\n", rtp->service_id);
		return RPAL_FAILURE;
	}

	if (!__atomic_compare_exchange_n(&requested_services[id].service,
					 &expected, rtp, 1, __ATOMIC_SEQ_CST,
					 __ATOMIC_SEQ_CST)) {
		errprint("rpal service %d already add, expected: %ld\n", id,
			 expected->service_key);
		return RPAL_FAILURE;
	}
	return RPAL_SUCCESS;
}

static rpal_thread_pool_t *get_service_from_key(uint64_t key)
{
	int i;
	rpal_thread_pool_t *rtp;

	for (i = 0; i < MAX_SERVICEID; i++) {
		rtp = requested_services[i].service;
		if (!rtp)
			continue;
		if (rtp->service_key == key) {
			return rtp;
		}
	}
	return NULL;
}

static inline rpal_thread_pool_t *get_service_from_id(int id)
{
	return requested_services[id].service;
}

static rpal_thread_pool_t *del_requested_service(uint64_t key)
{
	int id;
	rpal_thread_pool_t *rtp;

	rtp = get_service_from_key(key);
	if (!rtp)
		return NULL;
	id = rtp->service_id;
	rtp = __atomic_exchange_n(&requested_services[id].service, NULL,
				  __ATOMIC_RELAXED);
	return rtp;
}

int rpal_request_service(uint64_t key)
{
	rpal_thread_pool_t *rsm;
	long ret = RPAL_FAILURE;

	if (!rpal_inited()) {
		errprint("%s: rpal do not init\n", __FUNCTION__);
		goto error_out;
	}
	rsm = malloc(sizeof(rpal_thread_pool_t));
	if (!rsm) {
		errprint("%s: malloc rsm failed\n", __FUNCTION__);
		goto error_out;
	}
	ret = rpal_ctl(RPAL_CMD_REQUEST_SERVICE, (unsigned long)key,
		       (unsigned long)rsm);
	if (ret) {
		goto requested_failed;
	}

	ret = add_requested_service(rsm);
	if (ret == RPAL_FAILURE) {
		goto add_requested_failed;
	}

	return RPAL_SUCCESS;

add_requested_failed:
	rpal_ctl(RPAL_CMD_RELEASE_SERVICE, key, 1);
requested_failed:
	free(rsm);
error_out:
	return (int)ret;
}

static void fdt_freelist_forcefree(fd_table_t *fdt, uint64_t service_key);

status_t rpal_release_service(uint64_t key)
{
	long ret;
	rpal_thread_pool_t *rsm;

	if (!rpal_inited()) {
		errprint("%s: rpal do not init\n", __FUNCTION__);
		return RPAL_FAILURE;
	}

	rsm = del_requested_service(key);
	ret = rpal_ctl(RPAL_CMD_RELEASE_SERVICE, key, 1);
	if (ret) {
		errprint("rpal release service failed: %ld\n", ret);
		return RPAL_FAILURE;
	}
	fdt_freelist_forcefree(threads_md.recver_rtp->fdt, key);
	if (rsm) {
		free(rsm);
	}
	return RPAL_SUCCESS;
}

static void try_clean_lock(rpal_thread_info_t *rti, uint64_t key)
{
	uint64_t lock_state = key | 1UL << 63;

	if (__atomic_load_n(&rti->uqlock, __ATOMIC_RELAXED) == lock_state)
		uevent_queue_fix(&rti->ueventq);

	if (__atomic_compare_exchange_n(&rti->uqlock, &lock_state, (uint64_t)0,
					1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
		dbprint(RPAL_DEBUG_MANAGEMENT,
			"Serivce (key: %lu) does exit with holding lock\n",
			key);
}

struct release_info {
	uint64_t keys[KEY_SIZE];
	int size;
};

status_t rpal_clean_service_start(int64_t *ptr)
{
	rpal_thread_info_t *rti;
	struct release_info *info;
	int i, j;
	int size;

	if (!ptr) {
		goto error_out;
	}

	info = malloc(sizeof(struct release_info));
	if (info == NULL) {
		errprint("alloc release_info fail\n");
		goto error_out;
	}

	pthread_mutex_lock(&release_lock);
	size = read(rpal_mgtfd, info->keys, KEY_SIZE * sizeof(uint64_t));
	if (size <= 0) {
		errprint("Read keys on rpal_mgtfd failed\n");
		goto error_unlock;
	}

	size /= sizeof(uint64_t);
	info->size = size;

	for (i = 0; i < size; i++) {
		for (j = 0; j < threads_md.recver_rtp->nr_threads; j++) {
			rti = threads_md.recver_rtp->rtis + j;
			try_clean_lock(rti, info->keys[i]);
		}
	}
	pthread_mutex_unlock(&release_lock);
	*ptr = (int64_t)info;
	return RPAL_SUCCESS;

error_unlock:
	pthread_mutex_unlock(&release_lock);
	free(info);
error_out:
	return RPAL_FAILURE;
}

void rpal_clean_service_end(int64_t *ptr)
{
	int i;
	struct release_info *info;

	if (ptr == NULL)
		return;
	info = (struct release_info *)(*ptr);
	if (info == NULL)
		return;
	for (i = 0; i < info->size; i++) {
		dbprint(RPAL_DEBUG_MANAGEMENT, "release service: 0x%lx\n",
			info->keys[i]);
		rpal_release_service(info->keys[i]);
	}
	free(info);
}
int rpal_get_service_id(void)
{
	if (!rpal_inited()) {
		return RPAL_FAILURE;
	}
	return threads_md.service_id;
}

status_t rpal_get_service_key(uint64_t *service_key)
{
	if (!rpal_inited() || !service_key) {
		return RPAL_FAILURE;
	}
	*service_key = threads_md.service_key;
	return RPAL_SUCCESS;
}

int rpal_get_request_service_id(uint64_t key)
{
	rpal_thread_pool_t *rtp;

	rtp = get_service_from_key(key);
	if (!rtp) {
		return RPAL_FAILURE;
	}
	return rtp->service_id;
}

static fdt_node_t *fdt_node_alloc(fd_table_t *fdt)
{
	fdt_node_t *node;
	fd_event_t **ev;
	int *ref_count;
	uint16_t *timestamps;
	int size = 0;

	node = malloc(sizeof(fdt_node_t));
	if (!node)
		goto node_alloc_failed;

	size = sizeof(fd_event_t **) * (1 << fdt->node_shift);
	ev = malloc(size);
	if (!ev)
		goto events_alloc_failed;
	memset(ev, 0, size);

	size = sizeof(int) * (1 << fdt->node_shift);
	ref_count = malloc(size);
	if (!ref_count)
		goto used_alloc_failed;
	memset(ref_count, 0xff, size);

	size = sizeof(uint16_t) * (1 << fdt->node_shift);
	timestamps = malloc(size);
	if (!timestamps)
		goto ts_alloc_failed;
	memset(timestamps, 0, size);

	node->events = ev;
	node->ref_count = ref_count;
	node->next = NULL;
	node->timestamps = timestamps;
	if (!fdt->head) {
		fdt->head = node;
		fdt->tail = node;
	} else {
		fdt->tail->next = node;
		fdt->tail = node;
	}
	fdt->max_fd += (1 << fdt->node_shift);
	return node;

ts_alloc_failed:
	free(ref_count);
used_alloc_failed:
	free(ev);
events_alloc_failed:
	free(node);
node_alloc_failed:
	errprint("%s Error!!! max_fd: %d\n", __FUNCTION__, fdt->max_fd);
	return NULL;
}

static void fdt_node_free_all(fd_table_t *fdt)
{
	fdt_node_t *node, *ptr;

	node = fdt->head;
	while (node) {
		free(node->timestamps);
		free(node->ref_count);
		free(node->events);
		ptr = node;
		node = node->next;
		free(ptr);
	}
}

static fdt_node_t *fdt_node_expand(fd_table_t *fdt, int fd)
{
	fdt_node_t *node = NULL;
	while (fd >= fdt->max_fd) {
		node = fdt_node_alloc(fdt);
		if (!node)
			break;
	}
	return node;
}

static fdt_node_t *fdt_node_search(fd_table_t *fdt, int fd)
{
	fdt_node_t *node = NULL;
	int pos = 0;
	if (fd >= fdt->max_fd)
		return NULL;
	pos = fd >> fdt->node_shift;
	node = fdt->head;
	while (pos) {
		if (!node) {
			errprint(
				"fdt node search ERROR! fd: %d, pos: %d, fdt->max_fd: %d\n",
				fd, pos, fdt->max_fd);
			return NULL;
		}
		node = node->next;
		pos--;
	}
	return node;
}

static fd_table_t *fd_table_alloc(unsigned int node_shift)
{
	fd_table_t *fdt;
	pthread_mutexattr_t mattr;

	fdt = malloc(sizeof(fd_table_t));
	if (!fdt)
		return NULL;
	fdt->head = NULL;
	fdt->tail = NULL;
	fdt->max_fd = 0;
	fdt->node_shift = node_shift;
	fdt->node_mask = (1 << node_shift) - 1;
	fdt->freelist = NULL;
	pthread_mutex_init(&fdt->list_lock, NULL);

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
	pthread_mutex_init(&fdt->lock, &mattr);
	return fdt;
}

static void fd_table_free(fd_table_t *fdt)
{
	if (!fdt)
		return;
	fdt_node_free_all(fdt);
	free(fdt);
	return;
}

static inline fd_event_t *fd_event_alloc(int fd, int epfd,
					 struct epoll_event *event)
{
	fd_event_t *fde;
	uint64_t *qdata;

	fde = (fd_event_t *)malloc(sizeof(fd_event_t));
	if (!fde)
		return NULL;

	fde->fd = fd;
	fde->epfd = epfd;
	fde->epev = *event;
	fde->events = 0;
	fde->node = NULL;
	fde->next = NULL;
	fde->timestamp = 0;
	fde->service_key = 0;
	__atomic_store_n(&fde->outdated, (uint16_t)0, __ATOMIC_RELEASE);

	qdata = malloc(DEFAULT_QUEUE_SIZE * sizeof(uint64_t));
	if (!qdata) {
		errprint("malloc queue data failed\n");
		goto malloc_error;
	}
	if (rpal_queue_init(&fde->q, qdata, DEFAULT_QUEUE_SIZE)) {
		errprint("fde queue alloc failed, fd: %d\n", fd);
		goto init_error;
	}
	return fde;

init_error:
	free(qdata);
malloc_error:
	free(fde);
	return NULL;
}

static inline void fd_event_free(fd_event_t *fde)
{
	uint64_t *qdata;

	if (!fde)
		return;
	qdata = rpal_queue_destroy(&fde->q);
	free(qdata);
	free(fde);
	return;
}

static void fdt_freelist_insert(fd_table_t *fdt, fd_event_t *fde)
{
	if (!fde)
		return;

	pthread_mutex_lock(&fdt->list_lock);
	if (fdt->freelist == NULL) {
		fdt->freelist = fde;
	} else {
		fde->next = fdt->freelist;
		fdt->freelist = fde;
	}
	pthread_mutex_unlock(&fdt->list_lock);
}

static void fdt_freelist_forcefree(fd_table_t *fdt, uint64_t service_key)
{
	fd_event_t *prev, *pos, *f_fde;
	fdt_node_t *node;
	int idx;

	pthread_mutex_lock(&fdt->list_lock);
	prev = NULL;
	pos = fdt->freelist;
	while (pos) {
		idx = pos->fd & fdt->node_mask;
		node = pos->node;
		if (pos->service_key == service_key) {
			__atomic_exchange_n(&node->ref_count[idx], FDE_FREEING,
					    __ATOMIC_RELAXED);
			if (!prev) {
				fdt->freelist = pos->next;
			} else {
				prev->next = pos->next;
			}
			f_fde = pos;
			pos = pos->next;
			node->events[idx] = NULL;
			__atomic_store_n(&node->ref_count[idx], -1,
					 __ATOMIC_RELEASE);
			fd_event_free(f_fde);
		} else {
			prev = pos;
			pos = pos->next;
		}
	}
	pthread_mutex_unlock(&fdt->list_lock);
	return;
}

static void fdt_freelist_lazyfree(fd_table_t *fdt)
{
	fd_event_t *prev, *pos, *f_fde;
	fdt_node_t *node;
	int idx;
	int expected;

	pthread_mutex_lock(&fdt->list_lock);
	prev = NULL;
	pos = fdt->freelist;

	while (pos) {
		idx = pos->fd & fdt->node_mask;
		// do lazyfree when ref_count less than 0
		expected = FDE_AVAILABLE;
		node = pos->node;
		if (__atomic_compare_exchange_n(
			    &node->ref_count[idx], &expected, FDE_FREEING, 1,
			    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
			if (!prev) {
				fdt->freelist = pos->next;
			} else {
				prev->next = pos->next;
			}
			f_fde = pos;
			pos = pos->next;
			node->events[idx] = NULL;
			__atomic_store_n(&node->ref_count[idx], -1,
					 __ATOMIC_RELEASE);
			fd_event_free(f_fde);
		} else {
			if (expected < 0) {
				errprint("error ref: %d, fd: %d\n", expected,
					 pos->fd);
			}
			prev = pos;
			pos = pos->next;
		}
	}
	pthread_mutex_unlock(&fdt->list_lock);
	return;
}

static uint16_t fde_timestamp_get(fd_table_t *fdt, int fd)
{
	fdt_node_t *node;
	int idx;

	node = fdt_node_search(fdt, fd);
	if (!node) {
		return 0;
	}
	idx = fd & fdt->node_mask;
	return node->timestamps[idx];
}

static void fd_event_put(fd_table_t *fdt, fd_event_t *fde);

static fd_event_t *fd_event_get(fd_table_t *fdt, int fd)
{
	fd_event_t *fde = NULL;
	fdt_node_t *node;
	int idx;
	int val = -1;
	int expected;

	node = fdt_node_search(fdt, fd);
	if (!node) {
		return NULL;
	}
	idx = fd & fdt->node_mask;

retry:
	val = __atomic_load_n(&node->ref_count[idx], __ATOMIC_ACQUIRE);
	if (val < 0)
		return NULL;
	expected = val;
	val++;
	if (!__atomic_compare_exchange_n(&node->ref_count[idx], &expected, val,
					 1, __ATOMIC_SEQ_CST,
					 __ATOMIC_SEQ_CST)) {
		if (expected >= 0) {
			goto retry;
		} else {
			return NULL;
		}
	}
	fde = node->events[idx];
	if (!fde) {
		errprint("error get: %d, fd: %d\n", val, fd);
	} else {
		if (__atomic_load_n(&fde->outdated, __ATOMIC_ACQUIRE)) {
			fd_event_put(fdt, fde);
			fde = NULL;
		}
	}
	return fde;
}

static void fd_event_put(fd_table_t *fdt, fd_event_t *fde)
{
	int idx;
	int val;

	if (!fde)
		return;

	idx = fde->fd & fdt->node_mask;
	val = __atomic_sub_fetch(&fde->node->ref_count[idx], 1,
				 __ATOMIC_RELEASE);
	if (val < 0) {
		errprint("error put: %d, fd: %d\n", val, fde->fd);
	}
	return;
}

int rpal_access(void *addr, access_fn do_access, int *ret, va_list va);

int rpal_access(void *addr, access_fn do_access, int *ret, va_list va)
{
	int func_ret;

	func_ret = do_access(va);
	if (ret) {
		*ret = func_ret;
	}
	return RPAL_SUCCESS;
}

extern status_t rpal_access_warpper(void *addr, access_fn do_access, int *ret,
				    va_list va);

#define rpal_write_access_safety(ACCESS_FUNC, FUNC_RET, ...)                   \
	({                                                                     \
		status_t __access = RPAL_FAILURE;                              \
		uint32_t old_pkru = 0;                                         \
		old_pkru = rdpkru();                                           \
		__access = rpal_read_access_safety(ACCESS_FUNC, FUNC_RET,      \
						   ##__VA_ARGS__);             \
		wrpkru(old_pkru);                                              \
		__access;                                                      \
	})

status_t rpal_read_access_safety(access_fn do_access, int *ret, ...)
{
	rpal_sender_info_t *sender;
	rpal_app_context_t *app_stk;
	rpal_error_code_t error;
	status_t access = RPAL_FAILURE;
	va_list args;

	sender = current_rpal_sender();
	if (!sender || !rpal_sender_inited(sender)) {
		dbprint(RPAL_DEBUG_SENDER, "%s: sender(%d) do not init\n",
			__FUNCTION__, getpid());
		if (RPAL_FAILURE == rpal_sender_init(&error)) {
			return RPAL_FAILURE;
		}
		sender = current_rpal_sender();
	}
	app_stk = &sender->async_stk;
	app_stk->magic = RPAL_ERROR_MAGIC;
	va_start(args, ret);
	access = rpal_access_warpper(&(app_stk->ec.ersp), do_access, ret, args);
	va_end(args);
	app_stk->magic = 0;

	return access;
}

static int64_t __do_rpal_uds_fdmap(int service_id, int connfd)
{
	int64_t ret;

	ret = rpal_ctl(RPAL_CMD_UDS_FDMAP, service_id, connfd);
	if (ret < 0) {
		return RPAL_FAILURE;
	}
	return ret;
}

static status_t do_rpal_uds_fdmap(va_list va)
{
	int64_t ret;
	int sfd, cfd, sid;
	rpal_thread_pool_t *srtp;
	uint64_t stamp = 0;
	uint64_t sid_fd;
	uint64_t *rpalfd;
	fd_event_t *fde;

	sid_fd = va_arg(va, uint64_t);
	rpalfd = va_arg(va, uint64_t *);

	if (!rpalfd) {
		return RPAL_FAILURE;
	}
	sid = get_high32(sid_fd);
	cfd = get_low32(sid_fd);

	ret = __do_rpal_uds_fdmap(sid, cfd);
	if (ret < 0) {
		errprint("%s failed %ld, cfd: %d\n", __FUNCTION__, ret, cfd);
		return RPAL_FAILURE;
	}

	srtp = get_service_from_id(sid);
	if (!srtp) {
		errprint("%s INVALID service_id: %d\n", __FUNCTION__, sid);
		return RPAL_FAILURE;
	}
	sfd = get_sfd(ret);
	stamp = fde_timestamp_get(srtp->fdt, sfd);
	ret |= (stamp << HIGH16_OFFSET);

	fde = fd_event_get(threads_md.recver_rtp->fdt, cfd);
	if (!fde) {
		errprint("%s get self fde error, fd: %d\n", __FUNCTION__, cfd);
		goto out;
	}
	fde->service_key = srtp->service_key;
	fd_event_put(threads_md.recver_rtp->fdt, fde);
out:
	*rpalfd = ret;
	return RPAL_SUCCESS;
}

int rpal_get_peer_rid(uint64_t sid_fd)
{
	int64_t ret;
	int sid, cfd;
	int rid;

	sid = get_high32(sid_fd);
	cfd = get_low32(sid_fd);

	ret = __do_rpal_uds_fdmap(sid, cfd);
	if (ret < 0) {
		errprint("%s failed %ld, cfd: %d\n", __FUNCTION__, ret, cfd);
		return RPAL_FAILURE;
	}
	rid = get_rid(ret);
	return rid;
}

status_t rpal_uds_fdmap(uint64_t sid_fd, uint64_t *rpalfd)
{
	status_t ret = RPAL_FAILURE;
	status_t access;
	uint32_t old_pkru;

	old_pkru = rdpkru();
	wrpkru(old_pkru & RPAL_PKRU_BASE_CODE_READ);
	access = rpal_read_access_safety(do_rpal_uds_fdmap, &ret, sid_fd,
					 rpalfd);
	wrpkru(old_pkru);
	if (access == RPAL_FAILURE) {
		return RPAL_FAILURE;
	}
	return ret;
}

static status_t fd_event_install(fd_table_t *fdt, int fd, int epfd,
				 struct epoll_event *event)
{
	fdt_node_t *node;
	fd_event_t *fde;
	int idx;
	int expected;

	fde = fd_event_alloc(fd, epfd, event);
	if (!fde) {
		goto fde_error;
	}
	pthread_mutex_lock(&fdt->lock);
	if (fd >= fdt->max_fd) {
		node = fdt_node_expand(fdt, fd);
	} else {
		node = fdt_node_search(fdt, fd);
	}
	pthread_mutex_unlock(&fdt->lock);

	if (!node) {
		errprint("fd node search failed, fd: %d\n", fd);
		goto node_error;
	}
	idx = fd & fdt->node_mask;
	fdt_freelist_lazyfree(fdt);
	expected = __atomic_load_n(&node->ref_count[idx], __ATOMIC_ACQUIRE);
	if (expected != FDE_FREED) {
		goto node_error;
	}
	fde->timestamp =
		__atomic_add_fetch(&node->timestamps[idx], 1, __ATOMIC_RELEASE);
	fde->node = node;
	node->events[idx] = fde;
	if (!__atomic_compare_exchange_n(&node->ref_count[idx], &expected,
					 FDE_AVAILABLE, 1, __ATOMIC_SEQ_CST,
					 __ATOMIC_SEQ_CST)) {
		errprint("may override fd: %d, val: %d\n", fd, expected);
		node->events[idx] = NULL;
		goto node_error;
	}
	return RPAL_SUCCESS;

node_error:
	fd_event_free(fde);
fde_error:
	return RPAL_FAILURE;
}

static status_t fd_event_uninstall(fd_table_t *fdt, int fd)
{
	fd_event_t *fde;
	fdt_node_t *node;
	int idx;
	int ret = RPAL_SUCCESS;
	int expected;

	node = fdt_node_search(fdt, fd);
	if (!node) {
		ret = RPAL_FAILURE;
		goto out;
	}
	idx = fd & fdt->node_mask;
	fde = node->events[idx];
	if (!fde) {
		ret = RPAL_FAILURE;
		goto out;
	}
	expected = FDE_AVAILABLE;
	__atomic_store_n(&fde->outdated, (uint16_t)1, __ATOMIC_RELEASE);
	if (__atomic_compare_exchange_n(&node->ref_count[idx], &expected,
					FDE_FREEING, 1, __ATOMIC_SEQ_CST,
					__ATOMIC_SEQ_CST)) {
		node->events[idx] = NULL;
		__atomic_store_n(&node->ref_count[idx], -1, __ATOMIC_RELEASE);
		fd_event_free(fde);
	} else {
		if (expected < FDE_AVAILABLE) {
			errprint("error cnt: %d, fd: %d\n", expected, fde->fd);
		}
		// link this fde for free_head
		fdt_freelist_insert(fdt, fde);
	}

out:
	fdt_freelist_lazyfree(fdt);
	return ret;
}

static status_t fd_event_modify(fd_table_t *fdt, int fd,
				struct epoll_event *event)
{
	fd_event_t *fde;

	fde = fd_event_get(fdt, fd);
	if (!fde) {
		errprint("fde MOD fd(%d) ERROR!\n", fd);
		return RPAL_FAILURE;
	}
	fde->fd = fd;
	fde->epev = *event;
	fde->events = 0;
	fd_event_put(fdt, fde);
	return RPAL_SUCCESS;
}

static int rpal_thread_info_create(rpal_thread_pool_t *rtp, int id)
{
	rpal_thread_info_t *rti = &rtp->rtis[id];

	rti->ep_stack = fiber_ctx_alloc(NULL, NULL, DEFUALT_STACK_SIZE);
	if (!rti->ep_stack)
		return -1;

	rti->trampoline = fiber_ctx_alloc(NULL, NULL, TRAMPOLINE_SIZE);
	if (!rti->trampoline) {
		fiber_ctx_free(rti->ep_stack);
		return -1;
	}

	rti->async_epctx = threads_md.ep_contexts + id;
	rti->async_epctx->rid = id;
	rti->rtp = rtp;

	return 0;
}

static void rpal_thread_info_destroy(rpal_thread_info_t *rti)
{
	fiber_ctx_free(rti->ep_stack);
	fiber_ctx_free(rti->trampoline);
	return;
}

static rpal_thread_pool_t *rpal_thread_pool_create(int nr_threads,
						   rpal_thread_metadata_t *rtm)
{
	void *p;
	int i, j;
	rpal_thread_pool_t *rtp;

	if (rpal_inited())
		goto out;
	rtp = malloc(sizeof(rpal_thread_pool_t));
	if (rtp == NULL) {
		goto out;
	}
	threads_md.eventfds = malloc(nr_threads * sizeof(int));
	if (threads_md.eventfds == NULL) {
		goto eventfds_alloc_fail;
	}
	rtp->nr_threads = nr_threads;
	rtp->pkey = -1;
	p = malloc(nr_threads * sizeof(rpal_thread_info_t));
	if (p == NULL) {
		goto rti_alloc_fail;
	}
	rtp->rtis = p;
	memset(p, 0, nr_threads * sizeof(rpal_thread_info_t));

	rtp->fdt = fd_table_alloc(DEFAULT_NODE_SHIFT);
	if (!rtp->fdt) {
		goto fdt_alloc_fail;
	}

	p = rpal_get_shared_page(rtm->epcpage_order);

	if (!p)
		goto page_alloc_fail;
	rtm->ep_contexts = p;

	for (i = 0; i < nr_threads; i++) {
		if (rpal_thread_info_create(rtp, i)) {
			for (j = 0; j < i; j++) {
				rpal_thread_info_destroy(&rtp->rtis[j]);
			}
			goto rti_create_fail;
		}
	}
	return rtp;

rti_create_fail:
	rpal_free_shared_page(rtm->ep_contexts, rtm->epcpage_order);
page_alloc_fail:
	fd_table_free(rtp->fdt);
fdt_alloc_fail:
	free(rtp->rtis);
rti_alloc_fail:
	free(threads_md.eventfds);
eventfds_alloc_fail:
	free(rtp);
out:
	return NULL;
}

static void rpal_thread_pool_destory(rpal_thread_metadata_t *rtm)
{
	int i;
	rpal_thread_pool_t *rtp;

	if (!rpal_inited()) {
		errprint("thread pool is not created.\n");
		return;
	}
	pthread_mutex_destroy(&release_lock);
	rtp = threads_md.recver_rtp;
	fd_table_free(rtp->fdt);
	for (i = 0; i < rtp->nr_threads; ++i) {
		rpal_thread_info_destroy(&rtp->rtis[i]);
	}
	rpal_free_shared_page(threads_md.ep_contexts, threads_md.epcpage_order);
	free(rtp->rtis);
	free(threads_md.eventfds);
	free(rtp);
}

static inline int rpal_thread_inited(rpal_thread_info_t *rti)
{
	if (!rti)
		return 0;
	return (rti->status != RPAL_THREAD_UNINITIALIZED);
}

static inline int rpal_thread_available(rpal_thread_info_t *rti)
{
	return (rti->status == RPAL_THREAD_AVAILABLE);
}

static int rpal_thread_idx_get(void)
{
	return __atomic_fetch_add(&threads_md.rpal_thread_idx, 1,
				  __ATOMIC_RELAXED);
}

int rpal_thread_init(void)
{
	int ret = 0;
	int thread_idx;
	rpal_thread_info_t *rti;

	if (!rpal_inited()) {
		errprint("thread pool is not created.\n");
		goto error_out;
	}

	thread_idx = rpal_thread_idx_get();
	if (thread_idx >= threads_md.recver_rtp->nr_threads) {
		errprint(
			"rpal thread pool size exceeded. thread_idx: %d, thread pool capacity: %d\n",
			thread_idx, threads_md.recver_rtp->nr_threads);
		goto error_out;
	}

	rti = threads_md.recver_rtp->rtis + thread_idx;
	rti->status = RPAL_THREAD_UNINITIALIZED;
	rti->tid = syscall(SYS_gettid);
	rti->ser_tls_base = read_tls_base();

	rpal_uevent_queue_init(&rti->ueventq, &rti->uqlock);

	rti->async_epctx->rpal_ep_poll = 0;
	rti->async_epctx->ep_status = RPAL_EP_SYS;
	rti->async_epctx->ep_pending = 0;
	__atomic_store_n(&rti->async_epctx->g_status, RPAL_TASK_DONE,
			 __ATOMIC_RELAXED);
	ret = rpal_register_receiver(rti);
	if (ret < 0) {
		errprint("rpal thread %ld register failed %d\n", rti->tid, ret);
		goto error_out;
	}
	ret = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (ret < 0) {
		errprint("rpal thread %ld eventfd failed %d\n", rti->tid,
			 errno);
		goto eventfd_failed;
	}
	threads_md.eventfds[thread_idx] = ret;
	rti->status = RPAL_THREAD_INITIALIZED;
	return ret;

eventfd_failed:
	rpal_unregister_receiver();
error_out:
	return RPAL_FAILURE;
}

void rpal_thread_exit(void)
{
	rpal_thread_info_t *rti = current_rpal_thread();
	int id, fd;

	if (!rpal_thread_inited(rti))
		return;
	rti->status = RPAL_THREAD_UNINITIALIZED;
	id = rti->async_epctx->rid;
	fd = threads_md.eventfds[id];
	close(fd);
	threads_md.eventfds[id] = 0;
	rpal_unregister_receiver();
	return;
}

static inline void set_kcontext(volatile ksave_context_t *kc, void *src)
{
	fiber_stack_t *fstack = src;
	kc->r15 = fstack->r15;
	kc->r14 = fstack->r14;
	kc->r13 = fstack->r13;
	kc->r12 = fstack->r12;
	kc->rbx = fstack->rbx;
	kc->rbp = fstack->rbp;
	kc->rip = fstack->rip;
	kc->rsp = (unsigned long)(src + 0x40);
}

static transfer_t _syscall_epoll_wait(transfer_t t)
{
	rpal_thread_info_t *rti = t.ud;
	volatile rpal_epoll_context_t *ectx = rti->async_epctx;
	long ret;

	ectx->rpal_ep_poll = RPAL_EP_POLL_MAGIC;
	ret = epoll_wait(ectx->epfd, ectx->ep_events, ectx->ep_maxevents,
			 ectx->ep_timeout);
	t = jump_fcontext(rti->main_ctx, (void *)ret);
	return t;
}

extern void rpal_ret_critical(volatile rpal_epoll_context_t *ectx,
			      rpal_call_info_t *rci);

static transfer_t syscall_epoll_wait(transfer_t t)
{
	rpal_thread_info_t *rti = t.ud;
	volatile rpal_epoll_context_t *ectx = rti->async_epctx;
	rpal_call_info_t *rci = &rti->rci;
	task_t *estk = rti->ep_stack;

	set_kcontext(&rti->async_epctx->kcontext, t.fctx);
	rti->main_ctx = t.fctx;

	rpal_ret_critical(ectx, rci);

	estk->fctx = make_fcontext(estk->sp, 0, NULL);
	t = ontop_fcontext(rti->ep_stack->fctx, rti, _syscall_epoll_wait);
	return t;
}

static inline int ep_kernel_events_available(volatile int *ep_pending)
{
	return (RPAL_KERNEL_PENDING &
		__atomic_load_n(ep_pending, __ATOMIC_ACQUIRE));
}

static inline int ep_user_events_available(volatile int *ep_pending)
{
	return (RPAL_USER_PENDING &
		__atomic_load_n(ep_pending, __ATOMIC_ACQUIRE));
}

static inline int rpal_ep_send_events(epoll_uevent_queue_t *uq, fd_table_t *fdt,
				      volatile rpal_epoll_context_t *ectx,
				      struct epoll_event *events, int maxevents)
{
	int fd = -1;
	int ret = 0;
	int res = 0;
	fd_event_t *fde = NULL;

	__atomic_and_fetch(&ectx->ep_pending, ~RPAL_USER_PENDING,
			   __ATOMIC_ACQUIRE);
	while (uevent_queue_len(uq) && ret < maxevents) {
		fd = uevent_queue_del(uq);
		if (fd == -1) {
			errprint("uevent get failed\n");
			continue;
		}
		fde = fd_event_get(fdt, fd);
		if (!fde)
			continue;
		if (fde->events & EPOLLRPALIN) {
			RPAL_COUNT_INC(rpalin_events);
		}
		if (fde->events & EPOLLRPALOUT) {
			RPAL_COUNT_INC(rpalout_events);
		}
		res = __atomic_exchange_n(&fde->events, 0, __ATOMIC_RELAXED);
		res &= fde->epev.events;
		if (res) {
			events[ret].data = fde->epev.data;
			events[ret].events = res;
			ret++;
		}
		fd_event_put(fdt, fde);
	}
	if (uevent_queue_len(uq) || ret == maxevents) {
		dbprint(RPAL_DEBUG_RECVER,
			"uevent queue still have events, len: %d, ret: %d, maxevents: %d\n",
			uevent_queue_len(uq), ret, maxevents);
		__atomic_fetch_or(&ectx->ep_pending, RPAL_USER_PENDING,
				  __ATOMIC_RELAXED);
	}
	return ret;
}

extern void rpal_call_critical(volatile rpal_epoll_context_t *ectx,
			       rpal_thread_info_t *rti);

int rpal_epoll_wait(int epfd, struct epoll_event *events, int maxevents,
		    int timeout)
{
	transfer_t t;
	rpal_call_info_t *rci;
	task_t *estk, *trampoline;
	volatile rpal_epoll_context_t *ectx;
	epoll_uevent_queue_t *ueventq;
	rpal_thread_info_t *rti = current_rpal_thread();
	long ret = 0;
	unsigned int mxcsr = 0, fpucw = 0;

	if (!rpal_thread_inited(rti))
		return epoll_wait(epfd, events, maxevents, timeout);

	ectx = rti->async_epctx;
	estk = rti->ep_stack;
	trampoline = rti->trampoline;
	rci = &rti->rci;
	ueventq = &rti->ueventq;

	ectx->epfd = epfd;
	ectx->ep_events = events;
	ectx->ep_maxevents = maxevents;
	ectx->ep_timeout = timeout;

	if (!rpal_thread_available(rti)) {
		rti->status = RPAL_THREAD_AVAILABLE;
		estk->fctx = make_fcontext(estk->sp, 0, NULL);
		SAVE_FPU(mxcsr, fpucw);
		trampoline->fctx = make_fcontext(trampoline->sp, 0, NULL);
		t = ontop_fcontext(trampoline->fctx, rti, syscall_epoll_wait);
	} else {
		// kernel pending events
		if (ep_kernel_events_available(&ectx->ep_pending)) {
			ectx->rpal_ep_poll =
				RPAL_EP_POLL_MAGIC; // clear KERNEL_PENDING
			ret = epoll_wait(epfd, events, maxevents, 0);
			ectx->rpal_ep_poll = 0;
			goto send_user_events;
		}
		// user pending events
		if (ep_user_events_available(&ectx->ep_pending)) {
			goto send_user_events;
		}
		SAVE_FPU(mxcsr, fpucw);
		trampoline->fctx = make_fcontext(trampoline->sp, 0, NULL);
		t = ontop_fcontext(trampoline->fctx, rti, syscall_epoll_wait);
	}
	ectx->rpal_ep_poll = 0;

	/*
     * Here is where sender starts after user context switch.
     * The TLS may still be sender's. We should not do anything
     * that may use TLS, otherwise the result cannot be controlled.
     */

	switch (ectx->ep_status & RPAL_EP_STATUS_MASK) {
	case RPAL_EP_SYS: // syscall kernel ret
		ret = (long)t.ud;
		break;
	case RPAL_EP_KSYS: // receiver kernel ret
		RESTORE_FPU(mxcsr, fpucw);
		ret = (long)t.fctx;
		break;
	case RPAL_EP_APP: // rpalcall user jmp
		rci->app_tls_base = read_tls_base();
		rci->pkru = rdpkru();
		write_tls_base(rti->ser_tls_base);
		wrpkru(rpal_pkey_to_pkru(rti->rtp->pkey));
		rci->app_fctx = t.fctx;
		break;
	default:
		errprint("Error ep_status: %ld\n",
			 ectx->ep_status & RPAL_EP_STATUS_MASK);
		return -1;
	}

send_user_events:
	if (ret < maxevents && ret >= 0)
		ret += rpal_ep_send_events(ueventq, rti->rtp->fdt, ectx,
					   events + ret, maxevents - ret);
	return ret;
}

int rpal_epoll_wait_user(int epfd, struct epoll_event *events, int maxevents,
			 int timeout)
{
	volatile rpal_epoll_context_t *ectx;
	epoll_uevent_queue_t *ueventq;
	rpal_thread_info_t *rti = current_rpal_thread();

	if (!rpal_thread_inited(rti))
		return 0;

	if (!rpal_thread_available(rti))
		return 0;

	ectx = rti->async_epctx;
	ueventq = &rti->ueventq;
	if (ep_user_events_available(&ectx->ep_pending)) {
		return rpal_ep_send_events(ueventq, rti->rtp->fdt, ectx, events,
					   maxevents);
	}
	return 0;
}

int rpal_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	fd_table_t *fdt;
	int ret;

	ret = epoll_ctl(epfd, op, fd, event);
	if (ret || !rpal_inited()) {
		return ret;
	}
	fdt = threads_md.recver_rtp->fdt;
	switch (op) {
	case EPOLL_CTL_ADD:
		if (event->events & EPOLLRPALINOUT_BITS) {
			ret = fd_event_install(fdt, fd, epfd, event);
			if (ret == RPAL_FAILURE)
				goto install_error;
		}
		break;
	case EPOLL_CTL_MOD:
		fd_event_modify(fdt, fd, event);
		break;
	case EPOLL_CTL_DEL:
		fd_event_uninstall(fdt, fd);
		break;
	}
	return ret;
install_error:
	epoll_ctl(epfd, EPOLL_CTL_DEL, fd, event);
	return RPAL_FAILURE;
}

static transfer_t set_fcontext(transfer_t t)
{
	rpal_app_context_t *actx = t.ud;

	set_kcontext(&actx->kcontext, t.fctx);
	return t;
}

static void uq_lock(volatile uint64_t *uqlock, uint64_t key)
{
	uint64_t init = 0;

	while (1) {
		if (__atomic_compare_exchange_n(
			    uqlock, &init, (1UL << 63 | key), 1,
			    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			return;
		asm volatile("rep; nop");
		init = 0;
	}
}

static void uq_unlock(volatile uint64_t *uqlock)
{
	__atomic_store_n(uqlock, (uint64_t)0, __ATOMIC_RELAXED);
}

static status_t do_rpal_call_jump(rpal_sender_info_t *rsi,
				  rpal_thread_info_t *rti,
				  volatile rpal_epoll_context_t *ectx)
{
	int desired, expected;
	int64_t diff;

WAKE_AGAIN:
	desired = RPAL_BUILD_EP_APP(rsi->async_stk.sender_id,
				    threads_md.service_id);
	expected = RPAL_EP_WAIT;
	if (__atomic_compare_exchange_n(&ectx->ep_status, &expected, desired, 1,
					__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
		__atomic_store_n(&ectx->g_status, RPAL_TASK_BLOCKED,
				 __ATOMIC_RELAXED);
		RPAL_COUNT_INC(rpalcall_jmp);
		rsi->async_stk.start_time = _rdtsc();
		ontop_fcontext(rti->main_ctx, &rsi->async_stk, set_fcontext);

		if (__atomic_load_n(&ectx->g_status, __ATOMIC_RELAXED) ==
		    RPAL_TASK_DONE) {
			RPAL_COUNT_INC(rpalcall_ret);
			if (ectx->ep_status == RPAL_EP_KAPP)
				read(-1, NULL, 0);
			diff = _rdtsc() - rsi->async_stk.start_time;
			rsi->async_stk.total_time += diff;
			rti->async_epctx->total_time += diff;
			expected = desired;
			desired = RPAL_EP_WAIT;
			__atomic_compare_exchange_n(&ectx->ep_status, &expected,
						    desired, 1,
						    __ATOMIC_SEQ_CST,
						    __ATOMIC_SEQ_CST);

			if (ep_user_events_available(&ectx->ep_pending)) {
				RPAL_COUNT_INC(may_loss);
				goto WAKE_AGAIN;
			}
		} else {
			RPAL_COUNT_INC(sender_kernel_ret);
		}
		dbprint(RPAL_DEBUG_SENDER, "app return: 0x%x, %d, %d\n",
			ectx->ep_status, ectx->g_status, sfd);
	} else {
		RPAL_COUNT_INC(rpalcall_pending);
		RPAL_COUNT_ARRAY_INC(pending_reason,
				     ectx->ep_status & RPAL_EP_STATUS_MASK);
	}
	return RPAL_SUCCESS;
}

static inline void set_fde_trigger(fd_event_t *fde)
{
	__atomic_store_n(&fde->wait, FDE_TRIGGER_OUT, __ATOMIC_RELEASE);
	return;
}

static inline int clear_fde_trigger(fd_event_t *fde)
{
	int expected = FDE_TRIGGER_OUT;

	return __atomic_compare_exchange_n(&fde->wait, &expected,
					   FDE_NO_TRIGGER, 1, __ATOMIC_SEQ_CST,
					   __ATOMIC_SEQ_CST);
}

static int do_rpal_call(va_list va)
{
	rpal_sender_info_t *rsi;
	rpal_thread_info_t *rti;
	fd_event_t *fde;
	volatile rpal_epoll_context_t *ectx;
	rpal_thread_pool_t *srtp;
	uint16_t stamp;
	uint8_t rid;
	int sfd;
	int ret = 0;
	int fall = 0;

	int service_id = va_arg(va, int);
	uint64_t rpalfd = va_arg(va, uint64_t);
	int64_t *ptrs = va_arg(va, int64_t *);
	int len = va_arg(va, int);
	int flags = va_arg(va, int);

	rsi = current_rpal_sender();
	if (!rsi) {
		ret = RPAL_INVAL_THREAD;
		goto ERROR;
	}
	srtp = get_service_from_id(service_id);
	if (!srtp) {
		ret = RPAL_INVAL_SERVICE;
		goto ERROR;
	}

	rid = get_rid(rpalfd);
	sfd = get_sfd(rpalfd);
	rti = srtp->rtis + rid;
	if (!rti) {
		errprint("INVALID rid: %u, rti is NULL\n", rid);
		ret = RPAL_INVALID_ARG;
		goto ERROR;
	}
	wrpkru(rpal_pkru_union(rdpkru(), rpal_pkey_to_pkru(srtp->pkey)));
	ectx = rti->async_epctx;
	rsi->async_stk.ec.tls_base = rti->ser_tls_base;

	fde = fd_event_get(srtp->fdt, sfd);
	if (!fde) {
		ret = RPAL_INVALID_ARG;
		goto ERROR;
	}
	stamp = get_fdtimestamp(rpalfd);
	if (fde->timestamp != stamp) {
		ret = RPAL_FDE_OUTDATED;
		goto FDE_PUT;
	}

	uq_lock(&rti->uqlock, threads_md.service_key);
	if (uevent_queue_len(&rti->ueventq) == MAX_RDY) {
		errprint("rdylist is full: [%u, %u]\n", rti->ueventq.l_beg,
			 rti->ueventq.l_end);
		ret = RPAL_CACHE_FULL;
		goto UNLOCK;
	}
	if (likely(flags & RCALL_IN)) {
		if (unlikely(rpal_queue_unused(&fde->q) < (uint32_t)len)) {
			set_fde_trigger(fde);
			fall = 1;
			/* fall through: try to put data to queue */
		}
		ret = rpal_queue_put(&fde->q, ptrs, len);
		if (ret != len) {
			errprint("fde queue put error: %d, data: %lx\n", ret,
				 (unsigned long)fde->q.data);
			ret = RPAL_QUEUE_PUT_FAILED;
			goto UNLOCK;
		}
		if (unlikely(fall)) {
			clear_fde_trigger(fde);
		}
		fde->events |= EPOLLRPALIN;
	} else if (unlikely(flags & RCALL_OUT)) {
		ret = 0;
		fde->events |= EPOLLRPALOUT;
	} else {
		errprint("rpal call failed, ptrs: %lx, len: %d",
			 (unsigned long)ptrs, len);
		ret = RPAL_INVALID_ARG;
		goto UNLOCK;
	}

	uevent_queue_add(&rti->ueventq, sfd);
	uq_unlock(&rti->uqlock);
	fd_event_put(srtp->fdt, fde);

	__atomic_fetch_or(&ectx->ep_pending, RPAL_USER_PENDING,
			  __ATOMIC_RELEASE);
	do_rpal_call_jump(rsi, rti, ectx);
	return ret;

UNLOCK:
	uq_unlock(&rti->uqlock);
FDE_PUT:
	fd_event_put(srtp->fdt, fde);
ERROR:
	return -ret;
}

static int __rpal_write_ptrs_common(int service_id, uint64_t rpalfd,
				    int64_t *ptrs, int len, int flags)
{
	int ret = RPAL_FAILURE;
	status_t access = RPAL_FAILURE;

	if (unlikely(NULL == ptrs)) {
		dbprint(RPAL_DEBUG_SENDER, "%s: ptrs is NULL\n", __FUNCTION__);
		return -RPAL_INVALID_ARG;
	}
	if (unlikely(len <= 0 || ((uint32_t)len) > DEFAULT_QUEUE_SIZE)) {
		dbprint(RPAL_DEBUG_SENDER,
			"%s: data len less than or equal to zero\n",
			__FUNCTION__);
		return -RPAL_INVALID_ARG;
	}

	access = rpal_write_access_safety(do_rpal_call, &ret, service_id,
					  rpalfd, ptrs, len, flags);
	if (access == RPAL_FAILURE) {
		return -RPAL_ERR_PEER_MEM;
	}
	return ret;
}

int rpal_write_ptrs(int service_id, uint64_t rpalfd, int64_t *ptrs, int len)
{
	return __rpal_write_ptrs_common(service_id, rpalfd, ptrs, len,
					RCALL_IN);
}

int rpal_read_ptrs(int fd, int64_t *dptrs, int len)
{
	fd_event_t *fde;
	fd_table_t *fdt = threads_md.recver_rtp->fdt;
	int ret;

	if (!rpal_inited())
		return -1;

	fde = fd_event_get(fdt, fd);
	if (!fde)
		return -1;

	ret = rpal_queue_get(&fde->q, dptrs, len);
	fd_event_put(fdt, fde);
	return ret;
}

int rpal_read_ptrs_trigger_out(int fd, int64_t *dptrs, int len, int service_id,
			       uint64_t rpalfd)
{
	fd_event_t *fde;
	fd_table_t *fdt = threads_md.recver_rtp->fdt;
	int access, ret = -1;
	int nread;

	if (!rpal_inited())
		return -1;

	fde = fd_event_get(fdt, fd);
	if (!fde)
		return -1;

	nread = rpal_queue_get(&fde->q, dptrs, len);
	if (nread > 0 && clear_fde_trigger(fde)) {
		access =
			rpal_write_access_safety(do_rpal_call, &ret, service_id,
						 rpalfd, NULL, 0, RCALL_OUT);
		if (access == RPAL_FAILURE || ret < 0) {
			set_fde_trigger(fde);
			errprint(
				"trigger out failed! access: %d, ret: %d, id: %d, rpalfd: %lx\n",
				access, ret, service_id, rpalfd);
		}
	}
	fd_event_put(fdt, fde);

	return nread;
}

status_t rpal_copy_prepare(int service_id)
{
	rpal_thread_pool_t *rtp;
	uint32_t old_pkru;

	rtp = get_service_from_id(service_id);
	old_pkru = rdpkru();
	wrpkru(rpal_pkru_union(old_pkru, rpal_pkey_to_pkru(rtp->pkey)));
	return RPAL_SUCCESS;
}

status_t rpal_copy_finish(void)
{
	wrpkru(rpal_pkey_to_pkru(threads_md.recver_rtp->pkey));
	return RPAL_SUCCESS;
}

#ifdef RPAL_STATS
void rpal_recver_count_print(void)
{
	int i;
	uint32_t total_beg = 0;
	uint32_t total_end = 0;
	rpal_thread_info_t *rti;

	if (!rpal_inited())
		return;
	printf("\n**** rpal_recver_stats ******\n");
	RPAL_COUNT_PRINT(rpalin_events);
	RPAL_COUNT_PRINT(rpalout_events);
	RPAL_COUNT_PRINT(rpal_lazywake);

	for (i = 0; i < threads_md.recver_rtp->nr_threads; i++) {
		rti = threads_md.recver_rtp->rtis + i;
		total_beg += rti->ueventq.l_beg;
		total_end += rti->ueventq.l_end;

		printf("[%lu]:ueventq[%x, %x] = %x\n", rti->tid,
		       rti->ueventq.l_beg, rti->ueventq.l_end,
		       uevent_queue_len(&rti->ueventq));
	}
	printf("[ueventqs] [%x, %x] = %x\n", total_beg, total_end,
	       (total_end - total_beg));
	printf("**************************\n");
}

void rpal_sender_count_print(void)
{
	int i;
	printf("\n**** rpal_sender_stats ******\n");
	RPAL_COUNT_PRINT(rpalcall_jmp);
	RPAL_COUNT_PRINT(rpalcall_pending);

	printf("%-26s = %10x\n", "rpalcall total",
	       RPAL_COUNT_GET(rpalcall_jmp) + RPAL_COUNT_GET(rpalcall_pending));
	RPAL_COUNT_PRINT(rpalcall_ret);
	RPAL_COUNT_PRINT(sender_kernel_ret);
	RPAL_COUNT_PRINT(may_loss);

	for (i = 0; i < RPAL_EP_MAX; i++) {
		// printf("rpal_pending[%-7s]: %d\n", ep_status_s[i], rpal_pending[i]);
		RPAL_COUNT_ARRAY_PRINT(pending_reason, i, ep_status_s);
	}
	printf("**************************\n");
}
#else
void rpal_recver_count_print(void)
{
}
void rpal_sender_count_print(void)
{
}
#endif

static inline int pkey_is_invalid(const int pkey)
{
	return (pkey < 0 || pkey > 15);
}

static status_t rpal_thread_metadata_init(int nr_rpalthread,
					  rpal_error_code_t *error)
{
	uint64_t key;
	rpal_thread_pool_t *rtp;
	key = __rpal_get_service_key();
	if (key >= 1UL << 63) {
		ERRREPORT(
			error, RPAL_ERR_SERVICE_KEY,
			"rpal service key error. Service key: 0x%lx, oeverflow, should less than 2^63\n",
			key);
		goto error_out;
	}
	threads_md.service_key = key;
	threads_md.service_id = __rpal_get_service_id();
	pthread_mutex_init(&release_lock, NULL);
	rpal_get_critical_addr(&rcs);
	rtp = rpal_thread_pool_create(nr_rpalthread, &threads_md);
	if (rtp == NULL) {
		goto error_out;
	}
	threads_md.recver_rtp = rtp;
	if (rpal_enable_service(error) == RPAL_FAILURE)
		goto destroy_thread_pool;
	threads_md.pid = getpid();
	return RPAL_SUCCESS;

destroy_thread_pool:
	rpal_thread_pool_destory(&threads_md);
error_out:
	return RPAL_FAILURE;
}

static void rpal_thread_metadata_exit(void)
{
	rpal_disable_service();
	rpal_thread_pool_destory(&threads_md);
}

static status_t rpal_senders_metadata_init(rpal_error_code_t *error)
{
	if (senders_md) {
		ERRREPORT(error, RPAL_ERR_SENDERS_METADATA,
			  "senders metadata is already initialized.\n");
		return RPAL_FAILURE;
	}

	senders_md = malloc(sizeof(struct rpal_senders_metadata));
	if (!senders_md) {
		ERRREPORT(error, RPAL_ERR_NOMEM,
			  "senders metadata alloc failed.\n");
		goto sendes_alloc_failed;
	}
	senders_md->sdpage_order = SENDERS_PAGE_ORDER;
	memset(senders_md->bitmap, 0xFF,
	       sizeof(unsigned long) * BITS_TO_LONGS(MAX_SENDERS));
	pthread_mutex_init(&senders_md->lock, NULL);
	senders_md->senders = rpal_get_shared_page(senders_md->sdpage_order);
	if (!senders_md->senders) {
		ERRREPORT(error, RPAL_ERR_SENDER_PAGES,
			  "get senders share page error.\n");
		goto pages_alloc_failed;
	}
	dbprint(RPAL_DEBUG_MANAGEMENT, "senders pages addr: 0x%016lx\n",
		(unsigned long)senders_md->senders);
	return RPAL_SUCCESS;

pages_alloc_failed:
	free(senders_md);
sendes_alloc_failed:
	return RPAL_FAILURE;
}

static void rpal_senders_metadata_exit(void)
{
	if (!senders_md)
		return;

	rpal_free_shared_page((void *)senders_md->senders,
			      senders_md->sdpage_order);
	pthread_mutex_destroy(&senders_md->lock);
	free(senders_md);
}

static int rpal_get_version_cap(rpal_capability_t *version)
{
	return (int)rpal_ctl(RPAL_CMD_GET_API_VERSION_AND_CAP,
			     (unsigned long)version, 0);
}

static status_t rpal_version_check(rpal_capability_t *ver)
{
	if (ver->compat_version != MIN_RPAL_KERNEL_API_VERSION)
		return RPAL_FAILURE;
	if (ver->api_version < TARGET_RPAL_KERNEL_API_VERSION)
		return RPAL_FAILURE;
	return RPAL_SUCCESS;
}

static status_t rpal_capability_check(rpal_capability_t *ver)
{
	unsigned long cap = ver->cap;

	if (!(cap & (1 << RPAL_CAP_PKU))) {
		return RPAL_FAILURE;
	}

	if (!rpal_enabled_ptr)
		return RPAL_FAILURE;

	rpal_enabled = rpal_enabled_ptr;
	if (rpal_is_enabled_fast()) {
		return RPAL_SUCCESS;
	}
	return RPAL_FAILURE;
}

static status_t rpal_check_version_cap(rpal_error_code_t *error)
{
	int ret;

	ret = rpal_get_version_cap(&version);
	if (ret < 0) {
		ERRREPORT(error, RPAL_ERR_GET_CAP_VERSION,
			  "rpal get version failed: %d\n", ret);
		ret = RPAL_FAILURE;
		goto out;
	}
	ret = rpal_version_check(&version);
	if (ret == RPAL_FAILURE) {
		ERRREPORT(
			error, RPAL_KERNEL_API_NOTSUPPORT,
			"kernel rpal(version: %d-%d) API is not compatible with librpal(version: %d-%d)\n",
			version.compat_version, version.api_version,
			MIN_RPAL_KERNEL_API_VERSION,
			TARGET_RPAL_KERNEL_API_VERSION);
		goto out;
	}
	ret = rpal_capability_check(&version);
	if (ret == RPAL_FAILURE) {
		ERRREPORT(error, RPAL_HARDWARE_NOTSUPPORT,
			  "hardware do not support RPAL\n");
		goto out;
	}
out:
	return ret;
}

/* Must keep rpal_enabled_ptr != NULL */
int rpal_is_enabled_fast(void)
{
	return (*rpal_enabled == RPAL_ENABLED);
}

int rpal_is_enabled(void)
{
	if (!rpal_inited() || !rpal_enabled_ptr) {
		return 0;
	}
	return rpal_is_enabled_fast();
}

static status_t rpal_switch_init(rpal_error_code_t *error)
{
	int err;

	rpal_switchfd = open(RPAL_SWITCH_FILE, O_RDONLY);
	if (rpal_switchfd == -1) {
		err = errno;
		errprint("open %s fail, %d, %s\n", RPAL_SWITCH_FILE, err,
			 strerror(err));
		if (error) {
			*error = RPAL_ERR_RPALFILE_OPS;
		}
		goto error_out;
	}
	rpal_enabled_ptr =
		mmap(0, PAGE_SIZE, PROT_READ, MAP_SHARED, rpal_switchfd, 0);
	if (!rpal_enabled_ptr) {
		err = errno;
		errprint("mmap %s fail, %d, %s\n", RPAL_SWITCH_FILE, err,
			 strerror(err));
		if (error) {
			*error = RPAL_ERR_RPALFILE_OPS;
		}
		goto mmap_failed;
	}

	dbprint(RPAL_DEBUG_MANAGEMENT, "rpal_enabled_ptr: %x\n",
		*rpal_enabled_ptr);
	return RPAL_SUCCESS;

mmap_failed:
	close(rpal_switchfd);
error_out:
	return RPAL_FAILURE;
}

static void rpal_switch_destroy(void)
{
	int ret;
	if (!rpal_enabled_ptr) {
		return;
	}
	ret = munmap(rpal_enabled_ptr, PAGE_SIZE);
	if (ret) {
		errprint("rpal switch unmap failed: %d\n", ret);
	}
	close(rpal_switchfd);
	return;
}

static status_t rpal_mgtfd_init(rpal_error_code_t *error)
{
	int err, n;
	int mgtfd;
	char name[1024];

	mgtfd = open(RPAL_MGT_FILE, O_RDWR);
	if (mgtfd == -1) {
		err = errno;
		switch (err) {
		case EPERM:
			n = readlink("/proc/self/exe", name, sizeof(name) - 1);
			if (n < 0) {
				n = 0;
			}
			name[n] = 0;
			errprint("%s is not a RPAL binary\n", name);
			break;
		case ENOENT:
			errprint("Not in RPAL Environment\n");
			break;
		default:
			errprint("open %s fail, %d, %s\n", RPAL_MGT_FILE, err,
				 strerror(err));
		}
		if (error) {
			*error = RPAL_ERR_RPALFILE_OPS;
		}
		return RPAL_FAILURE;
	}
	rpal_mgtfd = mgtfd;
	return RPAL_SUCCESS;
}

static void rpal_mgtfd_destroy(void)
{
	if (rpal_mgtfd != -1) {
		close(rpal_mgtfd);
	}
	return;
}

#define RPAL_SECTION_SIZE (512 * 1024 * 1024 * 1024UL)

static inline status_t rpal_check_address(uint64_t start, uint64_t end,
					  uint64_t check)
{
	if (check >= start && check < end) {
		return RPAL_SUCCESS;
	}
	return RPAL_FAILURE;
}

static status_t rpal_managment_init(rpal_error_code_t *error)
{
	int i = 0;

	if (rpal_switch_init(error) == RPAL_FAILURE) {
		goto error_out;
	}
	if (rpal_mgtfd_init(error) == RPAL_FAILURE) {
		goto mgtfd_init_failed;
	}
	if (pthread_key_create(&rpal_key, NULL))
		goto rpal_key_failed;

	for (i = 0; i < MAX_SERVICEID; i++) {
		requested_services[i].service = NULL;
	}
	if (rpal_check_version_cap(error) == RPAL_FAILURE) {
		goto rpal_check_failed;
	}
	return RPAL_SUCCESS;

rpal_check_failed:
	pthread_key_delete(rpal_key);
rpal_key_failed:
	rpal_mgtfd_destroy();
mgtfd_init_failed:
	rpal_switch_destroy();
error_out:
	return RPAL_FAILURE;
}

static void rpal_managment_exit(void)
{
	pthread_key_delete(rpal_key);
	rpal_mgtfd_destroy();
	rpal_switch_destroy();
	return;
}

int rpal_init(int nr_rpalthread, int flags, rpal_error_code_t *error)
{
	if (nr_rpalthread <= 0) {
		dbprint(RPAL_DEBUG_MANAGEMENT,
			"%s: nr_rpalthread(%d) less than or equal to 0\n",
			__FUNCTION__, nr_rpalthread);
		return RPAL_FAILURE;
	}
	if (rpal_managment_init(error) == RPAL_FAILURE) {
		goto error_out;
	}
	if (rpal_thread_metadata_init(nr_rpalthread, error) == RPAL_FAILURE)
		goto managment_exit;

	if (rpal_senders_metadata_init(error) == RPAL_FAILURE)
		goto thread_md_exit;

	inited = 1;
	dbprint(RPAL_DEBUG_MANAGEMENT,
		"rpal init success, service key: 0x%lx, service id: %d, "
		"critical_start: 0x%016lx, critical_end: 0x%016lx\n",
		threads_md.service_key, threads_md.service_id, rcs.ret_begin,
		rcs.ret_end);
	return rpal_mgtfd;

thread_md_exit:
	rpal_thread_metadata_exit();
managment_exit:
	rpal_managment_exit();
error_out:
	return RPAL_FAILURE;
}

void rpal_exit(void)
{
	if (rpal_inited()) {
		dbprint(RPAL_DEBUG_MANAGEMENT,
			"rpal exit, service key: 0x%lx, service id: %d\n",
			threads_md.service_key, threads_md.service_id);
		rpal_senders_metadata_exit();
		rpal_thread_metadata_exit();
		rpal_managment_exit();
	}
}
