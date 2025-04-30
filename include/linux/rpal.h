/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RPAL_H_
#define _LINUX_RPAL_H_

#include <linux/hashtable.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/file.h>
#include <linux/page-flags.h>
#include <linux/binfmts.h>

#define RPAL_MAGIC "RPAL"
#define RPAL_MAGIC_OFFSET 12
#define RPAL_MAGIC_LEN 4

#define RPAL_ERROR_MSG "rpal error: "

#define rpal_err(x...) pr_err(RPAL_ERROR_MSG x)
#define rpal_err_ratelimited(x...) pr_err_ratelimited(RPAL_ERROR_MSG x)

/*
 * Process Virtual Address Space Layout (For 4-level Paging)
 *  |-------------|
 *  |  No Mapping |
 *  |-------------| <-- 64 KB (mmap_min_addr)
 *  |     ...     |
 *  |-------------| <-- 1 * 512GB
 *  |  service 0  |
 *  |-------------| <-- 2 * 512 GB
 *  |  Service 1  |
 *  |-------------| <-- 3 * 512 GB
 *  |  Service 2  |
 *  |-------------| <-- 4 * 512 GB
 *  |     ...     |
 *  |-------------| <-- 255 * 512 GB
 *  | Service 254 |
 *  |-------------| <-- 128 TB
 *  |             |
 *  |     ...     |
 *  |-------------| <-- PAGE_OFFSET
 *  |             |
 *  |    Kernel   |
 *  |_____________|
 *
 */
#define RPAL_ADDR_SPACE_SIZE (_AC(512, UL) * SZ_1G)
/* We need a randomize base mask due to too large randomize space. */
#define RPAL_RAND_ADDR_SPACE_MASK _AC(0xffffffff0, UL)
/* To calculate randomized space, We limit max randomized bits to 20 on frame number */
#define RPAL_MAX_RAND_BITS 20

#define RPAL_NR_ADDR_SPACE 256
#define RPAL_NR_PROCESS (RPAL_NR_ADDR_SPACE - 1)

#define RPAL_ADDRESS_SPACE_LOW                                                 \
		((RPAL_NR_ADDR_SPACE - RPAL_NR_PROCESS) * RPAL_ADDR_SPACE_SIZE)
#define RPAL_ADDRESS_SPACE_HIGH (RPAL_NR_ADDR_SPACE * RPAL_ADDR_SPACE_SIZE)

/*
 * The first 512GB is reserved due to mmap_min_addr.
 * The last 512GB is dropped since stack will be initially
 * allocated at TASK_SIZE_MAX.
 */
#define RPAL_NR_ID 254
#define RPAL_INVALID_ID -1

/* No more than 16 services can be requested due to limitation of MPK. */
#define MAX_REQUEST_SERVICE 16

/*
 * Following structures should have the same memory layout with user.
 * It seems nothing being different between kernel and user structure
 * padding by different C compilers on x86_64, so we need to do nothing
 * special here.
 */
/* Begin */
struct rpal_version_info {
	int compat_version;
	int api_version;
	unsigned long cap;
};

struct rpal_ctl_args {
	unsigned long __user *ret;
	unsigned long cmd;
	unsigned long arg0;
	unsigned long arg1;
};

struct rpal_service_metadata {
	void __user *infos;
	void __user *fdevs;
	u64 key;
	int nr_threads;
	int id;
	int pkey;
};

struct rpal_critical_section {
	unsigned long ret_begin;
	unsigned long ret_end;
};
/* End */

struct rpal_mapped_service {
	int pkey;
	unsigned long type;
	struct rpal_service *rs;
};

#define RPAL_MAX_SHARED_PAGES 1024
struct rpal_shared_page {
	unsigned long user_start;
	unsigned long user_end;
	unsigned long kernel_start;
	struct page *page;
	int npage;
	atomic_t refcnt;
	struct list_head list;
};

struct rpal_poll_data {
	spinlock_t poll_lock;
	u64 dead_keys[RPAL_NR_ID];
	DECLARE_BITMAP(dead_key_bitmap, RPAL_NR_ID);
	wait_queue_head_t rpal_waitqueue;
};

struct rpal_service {
	/* Fields below should never change after initialization. */
	char *name;
	/* The struct task_struct of thread group leader. */
	struct task_struct *leader_thread;
	/* address space id of the service which is allocate by rpal_alloc_id(). */
	int id;
	/* key which is unique to each service process */
	u64 key;
	/* base address of this service */
	unsigned long base;
	/* mm_struct of the service */
	struct mm_struct *mm;
	/* bad rpal binary */
	bool bad_service;
	bool enabled;

	/* map for services required, being required and mapped  */
	struct rpal_mapped_service service_map[RPAL_NR_ID];
	DECLARE_BITMAP(mapped_service_bitmap, RPAL_NR_ID);

	/* Fields below may change. */
	spinlock_t lock;
	/* Mutex for service level operations */
	struct mutex mutex;

	atomic_t req_avail_cnt;

	/* pinned page for sender and receiver */
	atomic_t nr_shared_pages;
	struct list_head shared_pages;

	/* Metadata that service provided to app. */
	struct rpal_service_metadata rsm;

	/* critical code section */
	struct rpal_critical_section rcs;

	/* delayed service put work */
	struct delayed_work delayed_put_work;

	/* Dead keys */
	struct rpal_poll_data rpd;

	/* Hash table list for this service */
	struct hlist_node hlist;
	atomic_t refcnt;
};

#define rpal_for_each_mapped_service(rs, idx)                                  \
	for (idx = find_first_bit(rs->mapped_service_bitmap, RPAL_NR_ID);      \
	     idx < RPAL_NR_ID; idx = find_next_bit(rs->mapped_service_bitmap,  \
						   RPAL_NR_ID, idx + 1))

static inline struct rpal_mapped_service *
rpal_get_mapped_node(struct rpal_service *rs, int id)
{
	return &rs->service_map[id];
}

struct rpal_service *rpal_get_service(struct rpal_service *rs);
void rpal_put_service(struct rpal_service *rs);

#if IS_ENABLED(CONFIG_RPAL)
static inline struct rpal_service *rpal_current_service(void)
{
	return current->rpal_rs;
}
void exit_rpal(bool group_dead);
void copy_rpal(struct task_struct *p);
#else
static inline struct rpal_service *rpal_current_service(void) { return NULL; }
static inline void exit_rpal(bool group_dead) { }
static inline void copy_rpal(struct task_struct *p) { }
#endif

static inline unsigned long rpal_get_base(struct rpal_service *rs)
{
	return rs->base;
}

static inline unsigned long rpal_get_top(struct rpal_service *rs)
{
	return rs->base + RPAL_ADDR_SPACE_SIZE;
}

/* service.c */
struct rpal_service *rpal_alloc_and_register_service(void);
void rpal_unregister_service(struct rpal_service *rs);

/* mm.c */
int rpal_balloon_init(unsigned long base);
void rpal_exit_mmap(struct mm_struct *mm);
bool rpal_is_correct_address(struct rpal_service *rs, unsigned long address);

void rpal_pick_mmap_base(struct mm_struct *mm, struct rlimit *rlim_stack);
#endif /* _LINUX_RPAL_H_ */
