// SPDX-License-Identifier: GPL-2.0-only
#include <linux/rpal.h>
#include <linux/slab.h>
#include <linux/bug.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>

#include "internal.h"

/*
 * Unconventionally, value '0' at rpal_id_bitmap means used while
 * '1' indicates the bit (id) is available.
 */
DECLARE_BITMAP(rpal_id_bitmap, RPAL_NR_ID);
struct kmem_cache *service_cache;
atomic64_t service_key_counter;
DEFINE_HASHTABLE(service_hash_table, ilog2(RPAL_NR_ID));
DEFINE_SPINLOCK(hash_table_lock);

static inline void rpal_free_service_id(int id)
{
	set_bit(id, rpal_id_bitmap);
}

static void __rpal_put_service(struct rpal_service *rs)
{
	pr_debug("rpal: free service %d, tgid: %d\n", rs->id,
		rs->leader_thread->pid);
	mmdrop(rs->mm);
	put_task_struct(rs->leader_thread);
	rpal_free_service_id(rs->id);
	kmem_cache_free(service_cache, rs);
}

void rpal_put_service_async_fn(struct work_struct *work)
{
	struct rpal_service *rs =
		container_of(work, struct rpal_service, delayed_put_work.work);

	__rpal_put_service(rs);
}

static int rpal_alloc_service_id(void)
{
	int id;

	do {
		id = find_first_bit(rpal_id_bitmap, RPAL_NR_ID);
		if (id == RPAL_NR_ID) {
			id = RPAL_INVALID_ID;
			break;
		}
	} while (!test_and_clear_bit(id, rpal_id_bitmap));

	return id;
}

static bool is_valid_id(int id)
{
	return id >= 0 && id < RPAL_NR_ID;
}

static u64 rpal_alloc_service_key(void)
{
	u64 key = atomic64_fetch_inc(&service_key_counter);
	/* confirm we do not run out keys */
	if (unlikely(key == _AC(-1, UL)))
		rpal_err("key is exhausted\n");
	return key;
}

/**
 * @brief get new reference to a rpal service, a corresponding
 *  rpal_put_service() should be called later by the caller.
 *
 * @param rs The struct rpal_service to get.
 *
 * @return new reference of struct rpal_service.
 */
struct rpal_service *rpal_get_service(struct rpal_service *rs)
{
	if (!rs)
		return NULL;
	atomic_inc(&rs->refcnt);
	return rs;
}

/**
 * @brief put a reference to a rpal service. If the reference count of
 *  the service turns to be 0, then release its struct rpal_service.
 *
 * @param rs The struct rpal_service to put.
 */
void rpal_put_service(struct rpal_service *rs)
{
	if (!rs)
		return;
	if (atomic_dec_and_test(&rs->refcnt)) {
		INIT_DELAYED_WORK(&rs->delayed_put_work,
				  rpal_put_service_async_fn);
		schedule_delayed_work(&rs->delayed_put_work, HZ * 30);
	}
}

static u32 get_hash_key(u64 key)
{
	return key % RPAL_NR_ID;
}

static struct rpal_service *rpal_get_service_by_key(u64 key)
{
	struct rpal_service *rs, *rsp;
	u32 hash_key = get_hash_key(key);

	rs = NULL;
	hash_for_each_possible(service_hash_table, rsp, hlist, hash_key) {
		if (rsp->key == key) {
			rs = rsp;
			break;
		}
	}
	return rpal_get_service(rs);
}

static void insert_service(struct rpal_service *rs)
{
	unsigned long flags;
	int hash_key;

	hash_key = get_hash_key(rs->key);

	spin_lock_irqsave(&hash_table_lock, flags);
	hash_add(service_hash_table, &rs->hlist, hash_key);
	spin_unlock_irqrestore(&hash_table_lock, flags);
}

static void delete_service(struct rpal_service *rs)
{
	unsigned long flags;

	spin_lock_irqsave(&hash_table_lock, flags);
	hash_del(&rs->hlist);
	spin_unlock_irqrestore(&hash_table_lock, flags);
}

static inline void set_mapped_service_bitmap(struct rpal_service *rs, int id)
{
	set_bit(id, rs->mapped_service_bitmap);
}

static inline void clear_mapped_service_bitmap(struct rpal_service *rs, int id)
{
	clear_bit(id, rs->mapped_service_bitmap);
}

static int add_mapped_service(struct rpal_service *rs, struct rpal_service *tgt,
			      int type_bit, int pkey)
{
	struct rpal_mapped_service *node;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&rs->lock, flags);
	node = rpal_get_mapped_node(rs, tgt->id);

	if (node->rs == NULL) {
		if (atomic_read(&rs->req_avail_cnt) == 0) {
			ret = -RPAL_ERR_REACH_LIMIT;
			goto unlock;
		}
		atomic_dec(&rs->req_avail_cnt);
		node->rs = rpal_get_service(tgt);
		set_bit(type_bit, &node->type);
		set_mapped_service_bitmap(rs, tgt->id);
	} else {
		if (node->rs != tgt) {
			ret = -RPAL_ERR_MAPPED;
			goto unlock;
		} else {
			if (test_and_set_bit(type_bit, &node->type)) {
				ret = -RPAL_ERR_INVAL;
				goto unlock;
			}
		}
	}

unlock:
	spin_unlock_irqrestore(&rs->lock, flags);
	return ret;
}

static void remove_mapped_service(struct rpal_service *rs, int id, int type_bit)
{
	struct rpal_mapped_service *node;
	struct rpal_service *t;
	unsigned long flags;

	spin_lock_irqsave(&rs->lock, flags);
	node = rpal_get_mapped_node(rs, id);
	if (node->rs == NULL)
		goto unlock;

	clear_bit(type_bit, &node->type);
	if (type_bit == RPAL_REQUEST_MAP)
		clear_bit(id, rs->rpd.dead_key_bitmap);

	if (node->type == 0) {
		clear_mapped_service_bitmap(rs, id);
		t = node->rs;
		node->rs = NULL;
		rpal_put_service(t);
		atomic_inc(&rs->req_avail_cnt);
	}

unlock:
	spin_unlock_irqrestore(&rs->lock, flags);
}

static int release_service(struct rpal_service *cur, struct rpal_service *tgt)
{
	if (unlikely(cur == tgt))
		rpal_err("%s: cur == tgt\n", __func__);

	remove_mapped_service(tgt, cur->id, RPAL_REVERSE_MAP);
	remove_mapped_service(cur, tgt->id, RPAL_REQUEST_MAP);
	rpal_unmap_service(tgt);

	return 0;
}

static void rpal_notify_disable(struct rpal_poll_data *rpd, u64 key, int id)
{
	unsigned long flags;
	bool need_wake = false;

	spin_lock_irqsave(&rpd->poll_lock, flags);
	if (!test_bit(id, rpd->dead_key_bitmap)) {
		need_wake = true;
		rpd->dead_keys[id] = key;
		set_bit(id, rpd->dead_key_bitmap);
	}
	spin_unlock_irqrestore(&rpd->poll_lock, flags);
	if (need_wake)
		wake_up_interruptible(&rpd->rpal_waitqueue);
}

static void rpal_release_service_all(void)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_service *tgt;
	int ret, i;

	rpal_for_each_mapped_service(cur, i) {
		struct rpal_mapped_service *node;

		if (i == cur->id)
			continue;
		node = rpal_get_mapped_node(cur, i);
		tgt = rpal_get_service(node->rs);
		if (!tgt)
			continue;

		if (test_bit(RPAL_REVERSE_MAP, &node->type))
			rpal_notify_disable(&tgt->rpd, cur->key, cur->id);

		if (test_bit(RPAL_REQUEST_MAP, &node->type)) {
			ret = release_service(cur, tgt);
			if (unlikely(ret)) {
				rpal_err("service %d release service %d fail\n",
					 cur->id, tgt->id);
			}
		}
		rpal_put_service(tgt);
	}
}

static bool ready_to_map(struct rpal_service *cur, int tgt_id)
{
	struct rpal_mapped_service *node;
	unsigned long flags;
	bool need_map = false;

	spin_lock_irqsave(&cur->lock, flags);
	node = rpal_get_mapped_node(cur, tgt_id);
	if (test_bit(RPAL_REQUEST_MAP, &node->type) &&
	    test_bit(RPAL_REVERSE_MAP, &node->type)) {
		need_map = true;
	}
	spin_unlock_irqrestore(&cur->lock, flags);

	return need_map;
}

long rpal_request_service(u64 key, void __user *to)
{
	struct rpal_service *cur, *tgt;
	struct rpal_service_metadata rsm;
	int pkey = 0;
	long ret = 0;
	int size;
	int id;

	cur = rpal_current_service();
	if (cur == NULL) {
		ret = -RPAL_ERR_NO_SERVICE;
		goto out;
	}

	size = sizeof(rsm);

	mutex_lock(&cur->mutex);

	if (!cur->enabled) {
		ret = -RPAL_ERR_BAD_SERVICE_STATUS;
		goto unlock_mutex;
	}

	if (cur->key == key) {
		ret = -RPAL_ERR_BAD_ARG;
		goto unlock_mutex;
	}

	tgt = rpal_get_service_by_key(key);
	if (tgt == NULL) {
		ret = -RPAL_ERR_NO_SERVICE;
		goto unlock_mutex;
	}

	if (!tgt->enabled) {
		ret = -RPAL_ERR_NO_SERVICE;
		goto put_service;
	}

	rsm = tgt->rsm;

	ret = copy_to_user(to, &rsm, size);
	if (ret) {
		ret = -RPAL_ERR_RETRY;
		goto put_service;
	}

	id = tgt->id;
	ret = add_mapped_service(cur, tgt, RPAL_REQUEST_MAP, pkey);
	if (ret < 0)
		goto put_service;

	ret = add_mapped_service(tgt, cur, RPAL_REVERSE_MAP, -1);
	if (ret < 0)
		goto remove_request;

	if (ready_to_map(cur, id)) {
		ret = rpal_map_service(tgt);
		if (ret < 0)
			goto remove_reverse;
	}

	mutex_unlock(&cur->mutex);

	rpal_put_service(tgt);

	return 0;

remove_reverse:
	remove_mapped_service(tgt, cur->id, RPAL_REVERSE_MAP);
remove_request:
	remove_mapped_service(cur, tgt->id, RPAL_REQUEST_MAP);
put_service:
	rpal_put_service(tgt);
unlock_mutex:
	mutex_unlock(&cur->mutex);
out:
	return ret;
}

long rpal_release_service(u64 key)
{
	struct rpal_service *cur, *tgt = NULL;
	unsigned long flags;
	long ret = 0;
	int i;

	cur = rpal_current_service();
	if (!cur) {
		ret = -RPAL_ERR_NO_SERVICE;
		goto out;
	}

	mutex_lock(&cur->mutex);

	if (cur->key == key) {
		ret = -RPAL_ERR_BAD_ARG;
		goto unlock_mutex;
	}

	spin_lock_irqsave(&cur->lock, flags);
	rpal_for_each_mapped_service(cur, i) {
		struct rpal_mapped_service *node;

		node = rpal_get_mapped_node(cur, i);
		if (node->rs->key == key) {
			tgt = rpal_get_service(node->rs);
			break;
		}
	}
	spin_unlock_irqrestore(&cur->lock, flags);

	if (!tgt) {
		ret = -RPAL_ERR_NO_SERVICE;
		goto unlock_mutex;
	}

	ret = release_service(cur, tgt);

	rpal_put_service(tgt);

unlock_mutex:
	mutex_unlock(&cur->mutex);
out:
	return ret;
}

static bool rpal_check_kdata(struct rpal_service *rs,
			     struct rpal_critical_section *rcs)
{
	return rpal_is_correct_address(rs, rcs->ret_begin) &&
	       rpal_is_correct_address(rs, rcs->ret_end);
}

long rpal_enable_service(void __user *u_data, void __user *k_data, bool is_new)
{
	struct rpal_service_metadata rsm;
	struct rpal_critical_section rcs;
	struct rpal_service *cur;
	unsigned long flags;
	int size;
	long ret = 0;

	size = sizeof(rsm);

	cur = rpal_current_service();
	if (!cur) {
		ret = -RPAL_ERR_NO_SERVICE;
		goto out;
	}

	if (cur->bad_service) {
		ret = -RPAL_ERR_BAD_SERVICE_STATUS;
		goto out;
	}

	ret = copy_from_user(&rsm, u_data, size);
	if (ret) {
		ret = -RPAL_ERR_RETRY;
		goto out;
	}

	ret = copy_from_user(&rcs, k_data, sizeof(rcs));
	if (ret) {
		ret = -RPAL_ERR_RETRY;
		goto out;
	}

	mutex_lock(&cur->mutex);

	if (!rpal_check_kdata(cur, &rcs)) {
		ret = -RPAL_ERR_BAD_ARG;
		goto unlock_mutex;
	}

	rsm.key = cur->key;
	rsm.id = cur->id;

	if (copy_to_user(u_data, &rsm, size)) {
		ret = -RPAL_ERR_RETRY;
		goto unlock_mutex;
	}

	spin_lock_irqsave(&cur->lock, flags);
	if (!cur->enabled) {
		cur->rsm = rsm;
		cur->rcs = rcs;
		cur->enabled = true;
	} else {
		ret = -RPAL_ERR_BAD_SERVICE_STATUS;
		spin_unlock_irqrestore(&cur->lock, flags);
		goto unlock_mutex;
	}
	spin_unlock_irqrestore(&cur->lock, flags);

	pr_debug("rpal debug: [%d] enable service %llx\n", current->pid,
		 cur->key);
unlock_mutex:
	mutex_unlock(&cur->mutex);
out:
	return ret;
}

long rpal_disable_service(void)
{
	struct rpal_service *cur;
	unsigned long flags;
	long ret = 0;

	cur = rpal_current_service();
	if (!cur) {
		ret = -RPAL_ERR_NO_SERVICE;
		goto out;
	}

	mutex_lock(&cur->mutex);

	spin_lock_irqsave(&cur->lock, flags);
	if (cur->enabled) {
		cur->enabled = false;
	} else {
		ret = -RPAL_ERR_BAD_SERVICE_STATUS;
		spin_unlock_irqrestore(&cur->lock, flags);
		goto unlock_mutex;
	}
	spin_unlock_irqrestore(&cur->lock, flags);

	rpal_release_service_all();

	pr_debug("rpal debug: [%d] disable service %llx\n", current->pid,
		 cur->key);

unlock_mutex:
	mutex_unlock(&cur->mutex);
out:
	return ret;
}

static void rpal_service_data_init(struct rpal_service *rs)
{
	spin_lock_init(&rs->lock);
	mutex_init(&rs->mutex);

	rs->base = 0;

	atomic_set(&rs->req_avail_cnt, MAX_REQUEST_SERVICE - 1);

	atomic_set(&rs->nr_shared_pages, 0);
	INIT_LIST_HEAD(&rs->shared_pages);

	rs->rcs.ret_begin = 0;
	rs->rcs.ret_end = 0;

	spin_lock_init(&rs->rpd.poll_lock);
	bitmap_zero(rs->rpd.dead_key_bitmap, RPAL_NR_ID);
	init_waitqueue_head(&rs->rpd.rpal_waitqueue);
}

static unsigned long calculate_base_address(int id)
{
	return RPAL_ADDRESS_SPACE_LOW + RPAL_ADDR_SPACE_SIZE * id;
}

static struct rpal_service *rpal_register_service(int service_id)
{
	struct rpal_mapped_service *node;
	struct rpal_service *rs;

	if (!thread_group_leader(current)) {
		rpal_err("task %d is not group leader %d\n", current->pid,
			 current->tgid);
		goto fail;
	}

	rs = kmem_cache_zalloc(service_cache, GFP_KERNEL);
	if (!rs)
		goto fail;

	rpal_service_data_init(rs);

	rs->leader_thread = get_task_struct(current);
	current->rpal_rs = rs;
	current->mm->rpal_rs = rs;
	rs->mm = current->mm;
	mmgrab(current->mm);

	rs->key = rpal_alloc_service_key();
	rs->bad_service = false;

	rs->id = service_id;
	rs->base = calculate_base_address(service_id);

	node = rpal_get_mapped_node(rs, service_id);
	node->rs = rs;
	set_bit(RPAL_REQUEST_MAP, &node->type);
	set_bit(RPAL_REVERSE_MAP, &node->type);
	bitmap_zero(rs->mapped_service_bitmap, RPAL_NR_ID);
	/*
	 * The reference comes from:
	 * 1. registered service always has one reference
	 * 2. leader_thread also has one reference
	 * 3. mm also hold one reference
	 */
	atomic_set(&rs->refcnt, 3);

	insert_service(rs);

	pr_debug("rpal: register service, key: %llx, id: %d, command: %s, tgid: %d\n",
		rs->key, rs->id, current->comm, current->tgid);

	return rs;

fail:
	rpal_free_service_id(service_id);
	return NULL;
}

void rpal_unregister_service(struct rpal_service *rs)
{
	struct rpal_mapped_service *node;
	unsigned long flags;

	if (!rs)
		return;

	delete_service(rs);
	spin_lock_irqsave(&rs->lock, flags);
	node = rpal_get_mapped_node(rs, rs->id);
	node->rs = NULL;
	node->pkey = 0;
	spin_unlock_irqrestore(&rs->lock, flags);
	if (unlikely(current->mm->rpal_rs != rs)) {
		rpal_err("current->mm->rpal_rs (0x%16lx) != rs (0x%16lx)\n",
			 (unsigned long)current->mm->rpal_rs,
			 (unsigned long)rs);
	}

	pr_debug("rpal: unregister service, id: %d, tgid: %d\n", rs->id,
		rs->leader_thread->tgid);

	rpal_put_service(rs);
}

extern const struct sched_class fair_sched_class;

struct rpal_service *rpal_alloc_and_register_service(void)
{
	int id = -1;

	if (!rpal_inited)
		return NULL;

	if (current->sched_class != &fair_sched_class) {
		rpal_err("Not fair sched class, pid: %d, comm: %s\n",
			 current->pid, current->comm);
		return NULL;
	}

	id = rpal_alloc_service_id();
	if (!is_valid_id(id))
		return NULL;

	return rpal_register_service(id);
}

void exit_rpal(bool group_dead)
{
	struct rpal_service *rs = rpal_current_service();

	if (!rs)
		return;

	if (group_dead)
		rpal_disable_service();

	if (group_dead)
		rpal_unregister_service(rs);
}

int __init rpal_service_init(void)
{
	service_cache = kmem_cache_create("rpal_service_cache",
					  sizeof(struct rpal_service), 0,
					  SLAB_PANIC, NULL);
	if (!service_cache) {
		rpal_err("service init fail\n");
		return -1;
	}

	bitmap_fill(rpal_id_bitmap, RPAL_NR_ID);
	atomic64_set(&service_key_counter, 1);

	return 0;
}

void rpal_service_exit(void)
{
	kmem_cache_destroy(service_cache);
}
