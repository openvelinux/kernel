// SPDX-License-Identifier: GPL-2.0
/*
 * Definitions for the NVM Express qmap interface
 * Copyright (c) 2024, Bytedance Corporation.
 */
#include <linux/jiffies.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <asm/param.h>		/* for HZ */
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/nvme-qmap.h>
#include "nvme.h"

static bool enable_at_startup;
module_param(enable_at_startup, bool, 0444);
MODULE_PARM_DESC(enable_at_startup, "enable qmap for all nvme ctrl at probe.");

static bool show_comptible_interface;
module_param(show_comptible_interface, bool, 0444);
MODULE_PARM_DESC(show_comptible_interface, "show queue_map interface.");

struct qmap_manager {
	u16 *comp_map;
	struct nvme_qmap *qmap_instance;
	const struct attribute *attr;
	/* protect comp_map,qmap_instance,and qmap relations */
	struct mutex qmap_lock;
};

#define MAX_QMAP	4096
struct qmap_manager qmap_mgr[MAX_QMAP];

struct {
	struct rw_semaphore list_rwsem;
	struct list_head	qmap_head;
} qmap_list;

#ifdef CONFIG_DEBUG_FS
struct dentry		*debug_nvme_qmap;
#endif

struct nvme_qmap_internal {
	struct nvme_qmap qmap;
	const struct nvme_ctrl_ops *ctrl_ops;
	const struct blk_mq_ops *tagset_ops;
	struct nvme_ctrl_ops nc_ops;
	struct blk_mq_ops bm_ops;
	struct nvme_queue *nq;
	u64 sq_st_size;
};

#define qmap_info(qmap, fmt, args...)	\
do {\
	BUILD_BUG_ON_MSG(!__same_type(*(qmap), struct nvme_qmap), "err point type");\
	dev_info(qmap->ctrl->device, "qmap:" fmt, ##args);\
} while (0)

#define qmap_err(qmap, fmt, args...)	\
do {\
	BUILD_BUG_ON_MSG(!__same_type(*(qmap), struct nvme_qmap), "err point type");\
	dev_err(qmap->ctrl->device, "qmap:" fmt, ##args);\
} while (0)

#define qmap_warn(qmap, fmt, args...)	\
do {\
	BUILD_BUG_ON_MSG(!__same_type(*(qmap), struct nvme_qmap), "err point type");\
	dev_warn(qmap->ctrl->device, "qmap:" fmt, ##args);\
} while (0)

/* for parallel module */
#ifndef QMAP_DEBUG_FS_DIR
#define QMAP_DEBUG_FS_DIR	"nvme_qmap"
#endif

static int nvme_qmap_init_qmap_mgr(void)
{
	int i;

	init_rwsem(&qmap_list.list_rwsem);
	INIT_LIST_HEAD(&qmap_list.qmap_head);

	for (i = 0; i < MAX_QMAP; i++)
		mutex_init(&qmap_mgr[i].qmap_lock);

#ifdef CONFIG_DEBUG_FS
	/* don't care if it fails */
	debug_nvme_qmap = debugfs_create_dir(QMAP_DEBUG_FS_DIR, NULL);
	if (IS_ERR(debug_nvme_qmap))
		pr_err("failed to create nvme_qmap debug dir.\n");
#endif /* CONFIG_DEBUG_FS */

	return 0;
}

static inline void nvme_qmap_uninit_qmap_mgr(void)
{
#ifdef CONFIG_DEBUG_FS
	if (!IS_ERR(debug_nvme_qmap))
		debugfs_remove_recursive(debug_nvme_qmap);
#endif /* CONFIG_DEBUG_FS */
}

static inline void nvme_qmap_mgr_set_attr(int instance, const struct attribute *attr)
{
	qmap_mgr[instance].attr = attr;
}

static const struct attribute *nvme_qmap_mgr_get_attr(int instance)
{
	return qmap_mgr[instance].attr;
}

static inline void nvme_qmap_mgr_add_qmap(struct nvme_qmap *qmap)
{
	down_write(&qmap_list.list_rwsem);
	list_add_tail(&qmap->qmap_entry, &qmap_list.qmap_head);
	up_write(&qmap_list.list_rwsem);

	qmap_mgr[qmap->ctrl->instance].comp_map = qmap->comp_map;
	qmap_mgr[qmap->ctrl->instance].qmap_instance = qmap;
}

static inline void nvme_qmap_mgr_remove_qmap(struct nvme_qmap *qmap)
{
	qmap_mgr[qmap->ctrl->instance].qmap_instance = NULL;
	qmap_mgr[qmap->ctrl->instance].comp_map = NULL;

	down_write(&qmap_list.list_rwsem);
	list_del(&qmap->qmap_entry);
	up_write(&qmap_list.list_rwsem);
}

struct nvme_qmap *nvme_qmap_mgr_get_by_instance(int instance)
{
	return qmap_mgr[instance].qmap_instance;
}
EXPORT_SYMBOL_GPL(nvme_qmap_mgr_get_by_instance);

static inline struct nvme_qmap *to_qmap(struct nvme_qmap_internal *qmap_inter)
{
	return (struct nvme_qmap *)qmap_inter;
}

static inline struct nvme_qmap_internal *to_internal(struct nvme_qmap *qmap)
{
	return (struct nvme_qmap_internal *)qmap;
}

static inline struct mutex *get_lock(struct nvme_ctrl *ctrl)
{
	return &qmap_mgr[ctrl->instance].qmap_lock;
}

static inline void nvme_qmap_clear_mq_map(struct blk_mq_queue_map *qmap)
{
	int cpu;

	for_each_possible_cpu(cpu)
		qmap->mq_map[cpu] = 0;
}

static int nvme_qmap_pci_map(struct blk_mq_queue_map *blk_mq_map,
			     struct nvme_qmap *qmap, int map_idx)
{
	const struct cpumask *mask;
	unsigned int queue, cpu, idx;
	struct nvme_qmap_set *sets = &qmap->sets;

	queue = blk_mq_map->queue_offset;

	if (queue + blk_mq_map->nr_queues > sets->nr_blk_mq) {
		qmap_err(qmap,
			 "Oops!! Bug detected.blk_mq queue idx overflow!");
		return 0;
	}

	for (idx = 0; idx < blk_mq_map->nr_queues; queue++, idx++) {
		mask = qmap->blk_mq_map[queue].mask;
		if (cpumask_first(mask) >= nr_cpu_ids) {
			qmap_err(qmap,
				 "Oops!! Bug detected.blk mq map not set!!");
			goto fallback;
		}
		for_each_cpu(cpu, mask) {
			blk_mq_map->mq_map[cpu] = queue;
		}
	}
	return 0;

fallback:
	WARN_ON_ONCE(blk_mq_map->nr_queues > 1);
	nvme_qmap_clear_mq_map(blk_mq_map);
	return 0;
}

struct nvme_qmap *nvme_qmap_get_qmap_from_ndev(struct nvme_dev *ndev)
{
	struct nvme_qmap *qmap = NULL;

	down_read(&qmap_list.list_rwsem);
	list_for_each_entry(qmap, &qmap_list.qmap_head, qmap_entry) {
		if (qmap->nvme_dev == ndev)
			break;
	}
	up_read(&qmap_list.list_rwsem);

	return qmap;
}
EXPORT_SYMBOL_GPL(nvme_qmap_get_qmap_from_ndev);

static int nvme_qmap_pci_map_queues(struct blk_mq_tag_set *set)
{
	void *ndev = set->driver_data;
	struct nvme_qmap *qmap;
	int i, qoff;

	qmap = nvme_qmap_get_qmap_from_ndev(ndev);

	for (i = 0, qoff = 0; i < set->nr_maps; i++) {
		struct blk_mq_queue_map *map = &set->map[i];

		map->nr_queues = qmap->sets.nr_ques[i];
		if (!map->nr_queues) {
			WARN_ON(i == HCTX_TYPE_DEFAULT);
			continue;
		}

		/*
		 * The poll queue(s) doesn't have an IRQ (and hence IRQ
		 * affinity), so use the regular blk-mq cpu mapping
		 */
		map->queue_offset = qoff;
		if (i != HCTX_TYPE_POLL)
			nvme_qmap_pci_map(map, qmap, i);
		else
			blk_mq_map_queues(map);
		qoff += map->nr_queues;
	}

	return 0;
}

static int hctx_to_nvme(struct nvme_qmap_mmap *mmap, int hctx_idx)
{
	return mmap[hctx_idx].nvme_qid;
}

static int nvme_qmap_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
			       unsigned int hctx_idx)
{
	struct nvme_dev *ndev = data;
	struct nvme_qmap *qmap;

	qmap = nvme_qmap_get_qmap_from_ndev(ndev);
	if (!qmap) {
		pr_err("qmap can't be find.\n");
		return 0;
	}

	hctx->driver_data =
		qmap->nvmeq[hctx_to_nvme(qmap->blk_mq_map, hctx->queue_num)];

	return 0;
}

static void update_driver_data(struct blk_mq_tag_set *set,
			       struct request_queue *q)
{
	struct blk_mq_hw_ctx **hctxs = q->queue_hw_ctx;
	struct nvme_dev *ndev = set->driver_data;
	struct nvme_queue *nvmeq;
	struct nvme_qmap *qmap;
	int i;

	qmap = nvme_qmap_get_qmap_from_ndev(ndev);

	for (i = 0; i < set->nr_hw_queues; i++) {
		nvmeq = qmap->nvmeq[hctx_to_nvme(qmap->blk_mq_map, hctxs[i]->queue_num)];
		hctxs[i]->driver_data = nvmeq;
	}
}

static void nvme_qmap_update_driver_data(struct blk_mq_tag_set *set)
{
	struct request_queue *q;

	mutex_lock(&set->tag_list_lock);
	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		update_driver_data(set, q);
	}
	mutex_unlock(&set->tag_list_lock);
}

static bool nvme_qmap_completed_rqs_timeout
(struct request *rq, void *data, bool reserved)
{
	unsigned int *count = data;

	if (blk_mq_request_completed(rq))
		(*count)++;
	return true;
}

static int nvme_qmap_wait_mq_completed_timeout
(struct blk_mq_tag_set *tagset, unsigned long timeout)
{
	unsigned long expiry;

	expiry = jiffies + timeout;

	while (true) {
		unsigned int count = 0;

		blk_mq_tagset_busy_iter(tagset,
					nvme_qmap_completed_rqs_timeout,
					&count);
		if (time_after(jiffies, expiry))
			return -1;
		if (!count)
			break;
		msleep(20);
	}
	return 0;
}

static int nvme_qmap_do_cease_io(struct nvme_ctrl *ctrl, unsigned long timeout)
{
	nvme_start_freeze(ctrl);
	timeout = nvme_wait_freeze_timeout(ctrl, timeout);
	if (timeout <= 0)
		return -ETIME;

	/* wait all hw queue dispatch finished */
	nvme_stop_queues(ctrl);

	/* wait all request completed from blk layer
	 * work like blk_mq_tagset_wait_completed_request
	 */
	return nvme_qmap_wait_mq_completed_timeout(ctrl->tagset, timeout);
}

static inline void nvme_qmap_restart_io(struct nvme_ctrl *ctrl)
{
	nvme_start_queues(ctrl);
	nvme_unfreeze(ctrl);
}

static int nvme_qmap_cease_io(struct nvme_ctrl *ctrl)
{
	int ret;

	ret = nvme_qmap_do_cease_io(ctrl, nvme_io_timeout * HZ);
	if (ret)
		nvme_qmap_restart_io(ctrl);

	return ret;
}

/* attention this must be the exactly one nmap, not the head one */
static inline int nvme_qmap_flag_enabled(struct nvme_qmap_nmap *nmap)
{
	return nmap->flag_disabled == QMAP_ENABLED;
}

static inline int nvme_qmap_flag_disabled(struct nvme_qmap_nmap *nmap)
{
	return nmap->flag_disabled == QMAP_DISABLED;
}

static inline int nvme_qmap_flag_occupied(struct nvme_qmap_nmap *nmap)
{
	return nmap->flag_disabled == QMAP_OCCUPIED;
}

static inline bool qid_in_default(struct nvme_qmap *qmap, int qid)
{
	int default_boundary = qmap->last_sets.nr_ques[HCTX_TYPE_DEFAULT];

	if (qid > 0 && qid <= default_boundary)
		return true;
	return false;
}

static inline bool qid_in_read(struct nvme_qmap *qmap, int qid)
{
	int read_start = qmap->last_sets.nr_ques[HCTX_TYPE_DEFAULT];
	int read_boundary = read_start + qmap->last_sets.nr_ques[HCTX_TYPE_READ];

	if (qid > read_start && qid <= read_boundary)
		return true;
	return false;
}

static inline bool qid_in_poll(struct nvme_qmap *qmap, int qid)
{
	int poll_start = qmap->last_sets.nr_ques[HCTX_TYPE_DEFAULT]
		+ qmap->last_sets.nr_ques[HCTX_TYPE_READ];
	int poll_boundary = poll_start + qmap->last_sets.nr_ques[HCTX_TYPE_POLL];

	if (qid > poll_start && qid <= poll_boundary)
		return true;
	return false;
}

static inline bool nvme_qmap_sets_check(struct nvme_qmap *qmap, int src, int dst)
{
	if (qid_in_default(qmap, src) && qid_in_default(qmap, dst))
		return true;
	if (qid_in_read(qmap, src) && qid_in_read(qmap, dst))
		return true;
	if (qid_in_poll(qmap, src) || qid_in_poll(qmap, dst)) {
		qmap_err(qmap, "poll queue not allowed.");
		return false;
	}
	qmap_err(qmap, "src:%d dst:%d not in the same set.",
		 src, dst);
	return false;
}

int nvme_qmap_wait_compl(struct nvme_ctrl *ctrl)
{
	if (mutex_lock_interruptible(get_lock(ctrl)))
		return -EINTR;

	if (ctrl->state != NVME_CTRL_LIVE) {
		mutex_unlock(get_lock(ctrl));
		return -EBUSY;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(nvme_qmap_wait_compl);

void nvme_qmap_finish_compl(struct nvme_ctrl *ctrl)
{
	mutex_unlock(get_lock(ctrl));
}
EXPORT_SYMBOL_GPL(nvme_qmap_finish_compl);

#define MAP_TYPE_DEFAULT	"Default"
#define MAP_TYPE_READ		"Read"
#define MAP_TYPE_POLL		"Poll"
#define MAP_TYPE_UNKNOWN	"Unknown"

static const char *nvme_qmap_get_nmap_type(struct nvme_qmap *qmap, int qid)
{
	if (qid_in_default(qmap, qid))
		return MAP_TYPE_DEFAULT;
	if (qid_in_read(qmap, qid))
		return MAP_TYPE_READ;
	if (qid_in_poll(qmap, qid))
		return MAP_TYPE_POLL;

	return MAP_TYPE_UNKNOWN;
}

/* for historical reason. We must transfer the id */
static inline int nqid2comptid(int qid)
{
	return qid - 1;
}

static inline int compatid2nqid(int comptid)
{
	return comptid + 1;
}

static inline int locate_word(const char *buf, int *start_off)
{
	int start, idx, str_len;

	idx = 0;
	while (buf[idx] == ' ' && buf[idx] != '\0')
		idx++;
	start = idx;
	while (buf[idx] != ' ' && buf[idx] != '\0')
		idx++;
	str_len = idx - start;
	*start_off = start;
	return str_len;
}

/* offset means the pos of next byte of cur word */
static inline char *find_word(const char *buf, int *end_off)
{
	int start, str_len;

	str_len = locate_word(buf, &start);
	if (str_len == 0) {
		*end_off = 0;
		return NULL;
	}

	/* idx points at the next char of cur word */
	*end_off = start + str_len;
	return kmemdup_nul(&buf[start], str_len, GFP_KERNEL);
}

static bool boundary_check(struct nvme_qmap *qmap, int qid)
{
	int nr_io_queues = qmap->last_sets.nr_blk_mq;

	if (qid < 1 || qid > nr_io_queues) {
		qmap_err(qmap, "qid:%d illegal.", qid);
		return false;
	}
	return true;
}

/* -1 means end of parse -2 means parse failed
 */
static inline int parse_qid(const char *buf, int *num, int *next_off)
{
	char *int_buf;
	int rc = 0;

	int_buf = find_word(buf, next_off);
	if (!int_buf)
		return -1;
	if (kstrtoint(int_buf, 10, num))
		rc = -2;
	kfree(int_buf);
	return rc;
}

#define MAX_NR_MAP 512

/* if greater than MAX_NR_MAP, we will cut it */
static int
nvme_qmap_gen_qid
(struct nvme_ctrl *ctrl, const char *buf, struct nvme_qmap_qid_param *param)
{
	const char *next_buf;
	int offset, idx, rc;
	int src_qid, dst_qid;

	next_buf = buf;
	offset = 0;

	param->map_arr =
	    kcalloc(MAX_NR_MAP, sizeof(*param->map_arr), GFP_KERNEL);
	if (!param->map_arr)
		return -ENOMEM;

	for (idx = 0; idx < MAX_NR_MAP; idx++) {
		next_buf += offset;
		rc = parse_qid(next_buf, &src_qid, &offset);
		/* end of parse */
		if (-1 == rc)
			break;

		if (rc != 0) {
			dev_err(ctrl->device, "qmap:failed to parse group:%d src.", idx);
			goto free_arr;
		}

		next_buf += offset;
		if (parse_qid(next_buf, &dst_qid, &offset)) {
			dev_err(ctrl->device, "qmap:failed to parse group:%d dst.", idx);
			goto free_arr;
		}
		param->map_arr[idx][0] = src_qid;
		param->map_arr[idx][1] = dst_qid;
	}

	if (idx == 0) {
		dev_err(ctrl->device, "qmap:no input pair detected.");
		goto free_arr;
	}
	param->nr_groups = idx;

	return 0;

free_arr:
	kfree(param->map_arr);
	param->map_arr = NULL;
	param->nr_groups = 0;

	return -EINVAL;
}

static inline void nvme_qmap_flag_enable(struct nvme_qmap_nmap *nmap)
{
	nmap->flag_disabled = QMAP_ENABLED;
}

static inline void nvme_qmap_flag_disable(struct nvme_qmap_nmap *nmap)
{
	nmap->flag_disabled = QMAP_DISABLED;
}

static inline void nvme_qmap_flag_occupy(struct nvme_qmap_nmap *nmap)
{
	nmap->flag_disabled = QMAP_OCCUPIED;
}

static int nvme_qmap_disable
(struct nvme_qmap *qmap, struct nvme_qmap_nmap *nmap, int src_qid, int dst_qid, int *incr)
{
	int orig_dst_qid, qid;

	if (!nvme_qmap_flag_enabled(&nmap[dst_qid])) {
		qmap_err(qmap, "dst qid:%d not enabled. can't be mapped.",
			 dst_qid);
		return -EINVAL;
	}

	/* by default the dst qid = src qid */
	orig_dst_qid = nmap[src_qid].dst_qid;
	if (orig_dst_qid == dst_qid) {
		qmap_err(qmap, "duplicated request.");
		return -EINVAL;
	}

	/* include src qid it self */
	for_each_qid(qid, &nmap[src_qid].qid_mask) {
		qidmask_set_qid(qid, &nmap[dst_qid].qid_mask);
		nmap[qid].dst_qid = dst_qid;
	}
	qidmask_clear_qid(src_qid, &nmap[orig_dst_qid].qid_mask);
	qidmask_clear(&nmap[src_qid].qid_mask);
	qidmask_set_qid(src_qid, &nmap[src_qid].qid_mask);

	/* tell upper layer not to decrese nr_blk_mq */
	if (nvme_qmap_flag_enabled(&nmap[src_qid])) {
		*incr = -1;
		nvme_qmap_flag_disable(&nmap[src_qid]);
	} else {
		*incr = 0;
	}

	return 0;
}

static int nvme_qmap_enable
(struct nvme_qmap *qmap, struct nvme_qmap_nmap *nmap, int qid, int *incr)
{
	int dst_qid;

	if (!nvme_qmap_flag_disabled(&nmap[qid])) {
		qmap_err(qmap, "qid:%d not disabled yet.", qid);
		return -EINVAL;
	}
	dst_qid = nmap[qid].dst_qid;
	qidmask_clear_qid(qid, &nmap[dst_qid].qid_mask);
	nmap[qid].dst_qid = qid;
	nvme_qmap_flag_enable(&nmap[qid]);
	*incr = 1;
	return 0;
}

static inline void nvme_qmap_dup_map
(struct nvme_qmap *qmap, struct nvme_qmap_nmap *src, struct nvme_qmap_nmap *dst)
{
	int idx, nr_queues;

	nr_queues = qmap->nr_queues;
	for (idx = 0; idx < nr_queues; idx++) {
		dst[idx].dst_qid = src[idx].dst_qid;
		dst[idx].flag_disabled = src[idx].flag_disabled;
		qidmask_copy(&dst[idx].qid_mask, &src[idx].qid_mask);
	}
}

static void nvme_qmap_dup_sets(struct nvme_qmap_set *src, struct nvme_qmap_set *dst)
{
	memcpy(dst->nr_ques, src->nr_ques, HCTX_MAX_TYPES * sizeof(*dst->nr_ques));
	dst->nr_blk_mq = src->nr_blk_mq;
}

#define DUP_DIR_FAIL	1
#define DUP_DIR_SUCCESS	0

static inline void nvme_qmap_dup
(struct nvme_qmap *qmap, int direction)
{
	struct nvme_qmap_nmap *snmap = qmap->shadow_nvme_map;
	struct nvme_qmap_nmap *nmap = qmap->nvme_map;
	struct nvme_qmap_set *sets = &qmap->sets;
	struct nvme_qmap_set *ssets = &qmap->shadow_sets;

	if (direction == DUP_DIR_SUCCESS) {
		nvme_qmap_dup_map(qmap, snmap, nmap);
		nvme_qmap_dup_sets(ssets, sets);
	} else {
		nvme_qmap_dup_map(qmap, nmap, snmap);
		nvme_qmap_dup_sets(sets, ssets);
	}
}

static int modify_shadow_map(struct nvme_qmap *qmap, int src, int dst, int *incr)
{
	int rc;
	struct nvme_qmap_nmap *shadow = qmap->shadow_nvme_map;

	if (src == dst)
		rc = nvme_qmap_enable(qmap, shadow, src, incr);
	else
		rc = nvme_qmap_disable(qmap, shadow, src, dst, incr);

	return rc;
}

static int nvme_qmap_modify_shadow_map(struct nvme_qmap *qmap, struct nvme_qmap_qid_param *param)
{
	struct nvme_qmap_set *shadow_sets = &qmap->shadow_sets;
	int rc, idx, src_qid, dst_qid, incr;

	for (idx = 0; idx < param->nr_groups; idx++) {
		src_qid = param->map_arr[idx][0];
		dst_qid = param->map_arr[idx][1];

		if (!boundary_check(qmap, src_qid) ||
		    !boundary_check(qmap, dst_qid)) {
			rc = -EINVAL;
			break;
		}

		if (!nvme_qmap_sets_check(qmap, src_qid, dst_qid)) {
			rc = -EINVAL;
			break;
		}

		rc = modify_shadow_map(qmap, src_qid, dst_qid, &incr);
		if (rc) {
			qmap_err(qmap, "illegal rule.");
			break;
		}

		if (qid_in_default(qmap, src_qid))
			shadow_sets->nr_ques[HCTX_TYPE_DEFAULT] += incr;
		if (qid_in_read(qmap, src_qid))
			shadow_sets->nr_ques[HCTX_TYPE_READ] += incr;
		if (qid_in_poll(qmap, src_qid))
			shadow_sets->nr_ques[HCTX_TYPE_POLL] += incr;
	}

	if (rc) {
		nvme_qmap_dup(qmap, DUP_DIR_FAIL);
		return rc;
	}

	shadow_sets->nr_blk_mq = shadow_sets->nr_ques[HCTX_TYPE_DEFAULT]
				+ shadow_sets->nr_ques[HCTX_TYPE_READ]
				+ shadow_sets->nr_ques[HCTX_TYPE_POLL];

	return 0;
}

static void nvme_qmap_clean_blk_mq_map(struct nvme_qmap *qmap)
{
	struct nvme_qmap_mmap *mmap = qmap->blk_mq_map;
	int i, nr_queues;

	nr_queues = qmap->nr_queues;
	for (i = 0; i < nr_queues; i++) {
		cpumask_clear(mmap[i].mask);
		mmap[i].nvme_qid = -1;
	}
}

/* core map logical for irq sets */
static void nvme_qmap_gen_map(struct nvme_qmap *qmap)
{
	struct pci_dev *pdev = to_pci_dev(qmap->ctrl->dev);
	struct nvme_qmap_nmap *nmap = qmap->nvme_map;
	struct nvme_qmap_mmap *mmap = qmap->blk_mq_map;
	struct nvme_qmap_set *last_sets = &qmap->last_sets;
	int mqid, nqid, tmp_qid, nr_nqs, rw_queues;
	u16 *comp_map = qmap->comp_map;

	rw_queues = last_sets->nr_ques[HCTX_TYPE_DEFAULT] +
				last_sets->nr_ques[HCTX_TYPE_READ];
	nr_nqs = rw_queues + last_sets->nr_ques[HCTX_TYPE_POLL];

	for (nqid = 1, mqid = 0; nqid <= nr_nqs; nqid++) {
		if (!nvme_qmap_flag_enabled(&nmap[nqid]))
			continue;
		for_each_qid(tmp_qid, &nmap[nqid].qid_mask) {
			comp_map[tmp_qid] = mqid;
			if (nqid > rw_queues)
				continue;
			cpumask_or(mmap[mqid].mask, mmap[mqid].mask,
				   pci_irq_get_affinity(pdev,
							tmp_qid));
		}
		mmap[mqid].nvme_qid = nqid;
		mqid++;
	}
}

/* core map logical */
static inline void nvme_qmap_generate_map(struct nvme_qmap *qmap)
{
	nvme_qmap_dup(qmap, DUP_DIR_SUCCESS);
	nvme_qmap_clean_blk_mq_map(qmap);
	nvme_qmap_gen_map(qmap);
}

static inline void nvme_qmap_free_qid_arr(struct nvme_qmap_qid_param *param)
{
	kfree(param->map_arr);
}

static inline struct blk_mq_tag_set *nvme_qmap_get_tagset(struct nvme_qmap *qmap)
{
	return qmap->ctrl->tagset;
}

static void nvme_qmap_do_map(struct nvme_qmap *qmap)
{
	struct blk_mq_tag_set *tagset = nvme_qmap_get_tagset(qmap);

	nvme_qmap_generate_map(qmap);
	blk_mq_update_nr_hw_queues(tagset, qmap->sets.nr_blk_mq);
	nvme_qmap_update_driver_data(tagset);
}

static int _nvme_qmap(struct nvme_ctrl *ctrl, struct nvme_qmap_qid_param *param)
{
	struct nvme_qmap *qmap;

	if (!nvme_qmap_mgr_enabled(ctrl->instance)) {
		dev_err(ctrl->device, "qmap:qmap not enabled.");
		return -EPERM;
	}

	if (!ctrl->tagset) {
		dev_err(ctrl->device, "qmap:no tagset.");
		return -EINVAL;
	}

	qmap = nvme_qmap_mgr_get_by_instance(ctrl->instance);

	if (nvme_qmap_modify_shadow_map(qmap, param))
		return -EINVAL;

	if (nvme_qmap_cease_io(ctrl)) {
		nvme_qmap_dup(qmap, DUP_DIR_FAIL);
		return -ETIME;
	}
	nvme_qmap_do_map(qmap);
	nvme_qmap_restart_io(ctrl);

	return 0;
}

static int nvme_qmap_map(struct nvme_ctrl *ctrl, struct nvme_qmap_qid_param *param)
{
	int rc;

	rc = nvme_qmap_wait_compl(ctrl);
	if (rc)
		return rc;

	rc = mutex_lock_interruptible(&ctrl->scan_lock);
	if (rc) {
		nvme_qmap_finish_compl(ctrl);
		return rc;
	}

	rc = _nvme_qmap(ctrl, param);
	mutex_unlock(&ctrl->scan_lock);
	nvme_qmap_finish_compl(ctrl);

	return rc;
}

int nvme_qmap_map_queues(struct nvme_ctrl *ctrl, struct nvme_qmap_qid_param *param)
{
	int ret;

	nvme_get_ctrl(ctrl);
	ret = nvme_qmap_map(ctrl, param);
	nvme_put_ctrl(ctrl);
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_qmap_map_queues);

static ssize_t qmap_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);
	struct nvme_qmap *qmap;
	struct nvme_qmap_nmap *nmap;
	const struct qidmask *mask;
	int idx, nr_queues;
	int size = 0;
	const char *type;
	int ret;

	ret = nvme_qmap_wait_compl(ctrl);
	if (ret)
		return ret;

	qmap = nvme_qmap_mgr_get_by_instance(ctrl->instance);

	nr_queues = qmap->last_sets.nr_blk_mq;

	/* start from 1 not 0 */
	for (idx = 1; idx <= nr_queues; idx++) {
		size += scnprintf(buf + size, PAGE_SIZE - size,
			"%d\t", idx);

		nmap = &qmap->nvme_map[idx];
		mask = &nmap->qid_mask;

		if (nvme_qmap_flag_occupied(nmap))
			size += scnprintf(buf + size, PAGE_SIZE - size,
					  "Occupy\t");

		if (nvme_qmap_flag_disabled(nmap))
			size += scnprintf(buf + size, PAGE_SIZE - size,
					  "Dead\t");

		if (nvme_qmap_flag_enabled(nmap))
			size += scnprintf(buf + size, PAGE_SIZE - size,
					  "%*pbl\t", mask->nr, mask->bits);

		size += scnprintf(buf + size, PAGE_SIZE - size,
				  "%d\t", nmap->dst_qid);

		type = nvme_qmap_get_nmap_type(qmap, idx);

		size += scnprintf(buf + size, PAGE_SIZE - size,
				  "%s\n", type);
	}

	nvme_qmap_finish_compl(ctrl);

	return size;
}

static ssize_t qmap_store
(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);
	struct nvme_qmap_qid_param param;
	int rc;

	if (ctrl->state != NVME_CTRL_LIVE) {
		dev_err(ctrl->device, "qmap:device stat not live.");
		return -EBUSY;
	}

	if (nvme_qmap_gen_qid(ctrl, buf, &param))
		return -EINVAL;

	rc = nvme_qmap_map(ctrl, &param);
	nvme_qmap_free_qid_arr(&param);

	if (rc)
		return rc;

	dev_info(ctrl->device, "qmap:remap accomplished.");
	return count;
}
static DEVICE_ATTR_RW(qmap);

static void queue_map_compat_change(struct nvme_qmap_qid_param *param)
{
	int i, src, dst, nr_groups;

	nr_groups = param->nr_groups;

	for (i = 0; i < nr_groups; i++) {
		src = param->map_arr[i][0];
		dst = param->map_arr[i][1];
		param->map_arr[i][0] = compatid2nqid(src);
		param->map_arr[i][1] = compatid2nqid(dst);
	}
}

static ssize_t queue_map_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	return qmap_show(dev, attr, buf);
}

static ssize_t queue_map_store
(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);
	struct nvme_qmap_qid_param param;
	int rc;

	if (ctrl->state != NVME_CTRL_LIVE) {
		dev_err(ctrl->device, "qmap:device stat not live.");
		return -EBUSY;
	}

	if (nvme_qmap_gen_qid(ctrl, buf, &param))
		return -EINVAL;

	/* for historical reson. We have to transfer it */
	queue_map_compat_change(&param);

	rc = nvme_qmap_map(ctrl, &param);
	nvme_qmap_free_qid_arr(&param);

	if (rc)
		return rc;

	dev_info(ctrl->device, "qmap:remap accomplished.");
	return count;
}
static DEVICE_ATTR_RW(queue_map);

#ifdef CONFIG_DEBUG_FS

#define precise_alloc(fmt, args...) kasprintf(GFP_KERNEL, fmt, ##args)

static const char *nvme_qmap_get_mmap_type(struct nvme_qmap *qmap, int mqid)
{
	struct nvme_qmap_set *set = &qmap->sets;
	u32 default_boundary = set->nr_ques[HCTX_TYPE_DEFAULT];
	u32 read_boundary = default_boundary + set->nr_ques[HCTX_TYPE_READ];
	u32 poll_boundary = read_boundary + set->nr_ques[HCTX_TYPE_POLL];

	if (mqid < default_boundary)
		return MAP_TYPE_DEFAULT;
	if (mqid < read_boundary)
		return MAP_TYPE_READ;
	if (mqid < poll_boundary)
		return MAP_TYPE_POLL;

	return MAP_TYPE_UNKNOWN;
}

static char *nvme_qmap_topo_nq_view(struct nvme_qmap *qmap,
				    struct nvme_qmap_nmap *top_nmap)
{
	struct nvme_ctrl *ctrl = qmap->ctrl;
	struct pci_dev *pdev = to_pci_dev(ctrl->dev);
	int nr_queues, rw_queues, idx;
	const struct cpumask *mask;
	const struct qidmask *qidmask;
	char *str, *tmp_buf;
	char *name, *cpu_list, *src_qid_list;

	rw_queues = qmap->last_sets.nr_ques[HCTX_TYPE_DEFAULT]
		+ qmap->last_sets.nr_ques[HCTX_TYPE_READ];
	nr_queues = rw_queues + qmap->last_sets.nr_ques[HCTX_TYPE_POLL];

	/* the header */
	str = precise_alloc("%12s\t%12s\t%12s\t%12s\t%12s\t%12s\t%12s\t%12s\n",
			    "idx", "nqid", "cpu_affinity", "src_nqid", "fake_did",
			    "dst_nqid", "dst_mqid", "group_type");

	/* nqid starts from 1 */
	for (idx = 1; idx <= nr_queues; idx++) {
		struct nvme_qmap_nmap *nmap = &top_nmap[idx];
		u16 dst_mqid = qmap->comp_map[idx];
		const char *type;

		name = precise_alloc("%d", nqid2comptid(idx));

		/* io queue start from 1 in msix table */
		if (idx <= rw_queues) {
			mask = pci_irq_get_affinity(pdev, idx);
			cpu_list = precise_alloc("%*pbl", cpumask_pr_args(mask));
		} else {
			cpu_list = precise_alloc("NULL");
		}

		qidmask = &nmap->qid_mask;
		if (nvme_qmap_flag_occupied(nmap))
			src_qid_list = precise_alloc("Occupy:%*pbl", qidmask->nr, qidmask->bits);
		else if (nvme_qmap_flag_disabled(nmap))
			src_qid_list = precise_alloc("Dead:%*pbl", qidmask->nr, qidmask->bits);
		else if (nvme_qmap_flag_enabled(nmap))
			src_qid_list = precise_alloc("%*pbl", qidmask->nr, qidmask->bits);
		else
			src_qid_list = precise_alloc("error");

		type = nvme_qmap_get_nmap_type(qmap, idx);

		tmp_buf =
		    precise_alloc("%s%12s\t%12d\t%12s\t%12s\t%12d\t%12d\t%12d\t%12s\n", str,
				  name, idx, cpu_list, src_qid_list,
				  nqid2comptid(nmap->dst_qid), nmap->dst_qid, dst_mqid, type);

		kfree(name);
		kfree(cpu_list);
		kfree(src_qid_list);
		kfree(str);

		if (!name || !cpu_list || !src_qid_list || !tmp_buf) {
			qmap_err(qmap, "no mem for nq view map.");
			kfree(tmp_buf);
			return NULL;
		}
		str = tmp_buf;
	}
	return str;
}

static char *nvme_qmap_topo_mq_view(struct nvme_qmap *qmap)
{
	struct nvme_qmap_mmap *mmap;
	int nr_queues, qid;
	char *str, *tmp_buf;
	char *affinity;
	const char *type;

	nr_queues = qmap->sets.nr_blk_mq;
	/* the header */
	str = precise_alloc("%12s\t%12s\t%12s\n",
			    "index", "cpu_affinity", "dst_nvme_qid");
	if (!str) {
		qmap_err(qmap, "not enough mem for blk mq veiw head.");
		return NULL;
	}

	for (qid = 0; qid < nr_queues; qid++) {
		mmap = &qmap->blk_mq_map[qid];
		affinity = precise_alloc("%*pbl", cpumask_pr_args(mmap->mask));
		type = nvme_qmap_get_mmap_type(qmap, qid);
		tmp_buf =
		    precise_alloc("%s%12d\t%12s\t%12d\t%12s\n", str, qid, affinity,
				  mmap->nvme_qid, type);
		kfree(affinity);
		kfree(str);
		if (NULL == affinity || NULL == tmp_buf) {
			qmap_err(qmap, "no mem for blk mq view map.");
			kfree(tmp_buf);
			return NULL;
		}
		str = tmp_buf;
	}
	return str;
}

static ssize_t nvme_qmap_debug_read(struct file *filp, char __user *buf,
				    size_t count, loff_t *pos)
{
	struct nvme_ctrl *ctrl = filp->private_data;
	struct nvme_qmap *qmap = nvme_qmap_mgr_get_by_instance(ctrl->instance);
	char *mq_view, *nq_view, *shadow, *str;
	size_t rc;

	rc = nvme_qmap_wait_compl(ctrl);
	if (rc)
		return rc;

	mq_view = nvme_qmap_topo_mq_view(qmap);
	nq_view =
	    nvme_qmap_topo_nq_view(qmap, qmap->nvme_map);
	shadow =
	    nvme_qmap_topo_nq_view(qmap, qmap->shadow_nvme_map);
	nvme_qmap_finish_compl(ctrl);

	str = precise_alloc("mq view\n%snq view\n%sshadow view\n%s\n",
			    mq_view, nq_view, shadow);
	if (!str) {
		rc = -1;
		goto free_str;
	}

	rc = simple_read_from_buffer
		(buf, count, pos, str, strlen(str));
free_str:
	kfree(mq_view);
	kfree(nq_view);
	kfree(shadow);
	kfree(str);
	return rc;
}

const struct file_operations nvme_qmap_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = nvme_qmap_debug_read,
};
#endif /* CONFIG_DEBUG_FS */

static void nvme_qmap_remove_files(struct nvme_qmap *qmap)
{
	if (!qmap->sysfs_added)
		return;

#ifdef CONFIG_DEBUG_FS
	debugfs_remove(qmap->dbg_file);
#endif

	if (show_comptible_interface)
		sysfs_remove_file_from_group(&qmap->ctrl->device->kobj,
					     &dev_attr_queue_map.attr,
					     NULL);

	sysfs_remove_file_from_group(&qmap->ctrl->device->kobj,
				     &dev_attr_qmap.attr, NULL);

	qmap->sysfs_added = false;
}

/* remove all of it in nvme_remove */
static int nvme_qmap_add_files(struct nvme_qmap *qmap)
{
	struct nvme_ctrl *ctrl = qmap->ctrl;

	if (qmap->sysfs_added)
		return 0;

	if (sysfs_add_file_to_group(&ctrl->device->kobj,
				    &dev_attr_qmap.attr, NULL)) {
		qmap_err(qmap, "failed to add sysfs entry:qmap.");
		return -ENODEV;
	}

	if (show_comptible_interface) {
		if (sysfs_add_file_to_group(&ctrl->device->kobj,
					    &dev_attr_queue_map.attr, NULL)) {
			sysfs_remove_file_from_group(&ctrl->device->kobj,
						     &dev_attr_qmap.attr,
						     NULL);
			qmap_err(qmap, "failed to add sysfs entry:queue_map.");
			return -ENODEV;
		}
	}

#ifdef CONFIG_DEBUG_FS
	qmap->dbg_file = debugfs_create_file(dev_name(ctrl->device),
					     0444, debug_nvme_qmap,
					     (void *)qmap->ctrl, &nvme_qmap_fops);
	/* don't care if it fails */
	if (IS_ERR(qmap->dbg_file))
		qmap_err(qmap, "failed to add debugfs for queue_map.");
#endif /* CONFIG_DEBUG_FS */

	qmap->sysfs_added = true;

	return 0;
}

static void nvme_qmap_init_default_nmap
(struct nvme_qmap *qmap, struct nvme_qmap_nmap *nvme_map)
{
	struct nvme_qmap_nmap *nmap;
	int i, nr_queues;

	nr_queues = qmap->nr_queues;
	for (i = 0; i < nr_queues; i++) {
		nmap = &nvme_map[i];
		qidmask_clear(&nmap->qid_mask);
		qidmask_set_qid(i, &nmap->qid_mask);
		nmap->dst_qid = i;
		nvme_qmap_flag_enable(nmap);
	}
}

static void nvme_qmap_init_sets(struct nvme_qmap *qmap, struct nvme_qmap_set *sets)
{
	struct nvme_ctrl *ctrl = qmap->ctrl;
	struct blk_mq_tag_set *tagset = ctrl->tagset;
	struct blk_mq_queue_map *map;
	unsigned int i;

	sets->nr_blk_mq = 0;
	memset(sets, 0, sizeof(*sets));

	for (i = 0; i < HCTX_MAX_TYPES; i++) {
		map = &tagset->map[i];
		sets->nr_ques[i] = map->nr_queues;
		sets->nr_blk_mq += map->nr_queues;
	}
}

/* this is the default map
 * need after setup_io_queue and before nvme_dev_add
 */
static void nvme_qmap_init_default_map(struct nvme_qmap *qmap)
{
	nvme_qmap_init_sets(qmap, &qmap->last_sets);
	nvme_qmap_init_sets(qmap, &qmap->shadow_sets);
	nvme_qmap_init_default_nmap(qmap, qmap->shadow_nvme_map);
	nvme_qmap_generate_map(qmap);
}

static void nvme_qmap_cache_blk_mq_ops(struct nvme_qmap *qmap)
{
	struct nvme_ctrl *ctrl = qmap->ctrl;
	struct nvme_qmap_internal *qmap_inter = to_internal(qmap);
	struct blk_mq_tag_set *tagset = ctrl->tagset;
	const struct blk_mq_ops *ops = tagset->ops;

	qmap_inter->tagset_ops = ops;
	memcpy(&qmap_inter->bm_ops, ops, sizeof(*ops));
	qmap_inter->bm_ops.init_hctx = nvme_qmap_init_hctx;
	qmap_inter->bm_ops.map_queues = nvme_qmap_pci_map_queues;
}

static inline void nvme_qmap_replace_blk_mq_ops(struct nvme_qmap *qmap)
{
	struct blk_mq_tag_set *tagset = nvme_qmap_get_tagset(qmap);
	struct nvme_qmap_internal *qmap_inter = to_internal(qmap);

	tagset->ops = &qmap_inter->bm_ops;
}

static inline void nvme_qmap_restore_blk_mq_ops(struct nvme_qmap *qmap)
{
	struct nvme_qmap_internal *qmap_inter = to_internal(qmap);
	struct blk_mq_tag_set *tagset = nvme_qmap_get_tagset(qmap);

	tagset->ops = qmap_inter->tagset_ops;
}

static void nvme_qmap_free_qmap(struct nvme_qmap *qmap)
{
	int nr_queues = qmap->nr_queues;
	struct nvme_qmap_nmap *nmap = qmap->nvme_map;
	struct nvme_qmap_nmap *snmap = qmap->shadow_nvme_map;
	struct nvme_qmap_mmap *mmap = qmap->blk_mq_map;
	int idx;

	for (idx = 0; idx < nr_queues; idx++) {
		free_qidmask(&nmap[idx].qid_mask);
		free_qidmask(&snmap[idx].qid_mask);
		free_cpumask_var(mmap[idx].mask);
	}
	kfree(qmap->nvmeq);
	kfree(qmap->shadow_nvme_map);
	kfree(qmap->nvme_map);
	kfree(qmap->comp_map);
	kfree(qmap->blk_mq_map);
	kfree(qmap);
}

static bool nvme_qmap_sets_same(struct nvme_qmap *qmap, struct nvme_qmap_set *last_sets)
{
	struct blk_mq_tag_set *tagset = qmap->ctrl->tagset;
	struct blk_mq_queue_map *map;
	int i;

	for (i = 0; i < HCTX_MAX_TYPES; i++) {
		map = &tagset->map[i];
		if (last_sets->nr_ques[i] != map->nr_queues) {
			qmap_warn(qmap, "qsets:%d not same.orig:%u vs now:%u",
				  i, last_sets->nr_ques[i], map->nr_queues);
			return false;
		}
	}
	return true;
}

static struct nvme_qmap *nvme_qmap_alloc
(struct nvme_ctrl *ctrl, struct nvme_queue *nq, u64 size, unsigned int nr_queues)
{
	struct nvme_qmap *qmap;
	struct nvme_qmap_internal *qmap_inter;
	int idx, node;

	node = dev_to_node(ctrl->dev);

	qmap_inter = kcalloc_node(1, sizeof(struct nvme_qmap_internal), GFP_KERNEL, node);
	if (!qmap_inter)
		return NULL;
	qmap = to_qmap(qmap_inter);

	qmap->ctrl = ctrl;
	qmap->nvme_dev = dev_get_drvdata(ctrl->dev);
	qmap->nr_queues = nr_queues;
	qmap_inter->nq = nq;
	qmap_inter->sq_st_size = size;

	qmap->blk_mq_map =
		kcalloc_node(nr_queues, sizeof(*qmap->blk_mq_map), GFP_KERNEL, node);
	qmap->comp_map =
		kcalloc_node(nr_queues, sizeof(*qmap->comp_map), GFP_KERNEL, node);
	qmap->nvme_map =
		kcalloc_node(nr_queues, sizeof(*qmap->nvme_map), GFP_KERNEL, node);
	qmap->shadow_nvme_map =
		kcalloc_node(nr_queues, sizeof(*qmap->nvme_map), GFP_KERNEL, node);
	qmap->nvmeq =
		kcalloc_node(nr_queues, sizeof(qmap->nvmeq), GFP_KERNEL, node);

	if (!qmap->blk_mq_map || !qmap->comp_map || !qmap->nvme_map ||
	    !qmap->shadow_nvme_map || !qmap->nvmeq) {
		dev_err(ctrl->device, "qmap:kalloc map failed.");
		goto err_free_map;
	}

	for (idx = 0; idx < nr_queues; idx++) {
		if (!zalloc_qidmask
			(&qmap->nvme_map[idx].qid_mask, nr_queues) ||
			!zalloc_qidmask
			(&qmap->shadow_nvme_map[idx].qid_mask, nr_queues) ||
			!zalloc_cpumask_var
			(&qmap->blk_mq_map[idx].mask, GFP_KERNEL)) {
			dev_err(ctrl->device, "qmap:zalloc qid mask failed.");
			goto err_free_mask;
		}
	}

	return qmap;

err_free_mask:
	for (idx = 0; idx < nr_queues; idx++) {
		free_cpumask_var(qmap->blk_mq_map[idx].mask);
		free_qidmask(&qmap->nvme_map[idx].qid_mask);
		free_qidmask(&qmap->shadow_nvme_map[idx].qid_mask);
	}
err_free_map:
	kfree(qmap->nvme_map);
	kfree(qmap->shadow_nvme_map);
	kfree(qmap->comp_map);
	kfree(qmap->blk_mq_map);
	kfree(qmap->nvmeq);
	kfree(qmap);

	return NULL;
}

/* cache all nvme queue no matter used or not */
static void nvme_qmap_cache_nvme_queues(struct nvme_qmap *qmap)
{
	struct nvme_qmap_internal *qmap_inter = to_internal(qmap);
	int i, nr_queues;
	u64 ptr_nq;
	u64 st_size;

	nr_queues = qmap->nr_queues;
	ptr_nq = (u64)qmap_inter->nq;
	st_size = qmap_inter->sq_st_size;

	for (i = 0; i < nr_queues; i++)
		qmap->nvmeq[i] = (struct nvme_queue *)(ptr_nq + i * st_size);
}

static void restore_driver_data(struct blk_mq_tag_set *set,
				struct request_queue *q)
{
	struct blk_mq_hw_ctx **hctxs = q->queue_hw_ctx;
	struct nvme_dev *ndev = set->driver_data;
	struct nvme_queue *nvmeq;
	struct nvme_qmap *qmap;
	int i;

	qmap = nvme_qmap_get_qmap_from_ndev(ndev);

	for (i = 0; i < set->nr_hw_queues; i++) {
		nvmeq = qmap->nvmeq[hctxs[i]->queue_num + 1];
		hctxs[i]->driver_data = nvmeq;
	}
}

static void nvme_qmap_restore_tagset(struct nvme_qmap *qmap)
{
	struct blk_mq_tag_set *set = nvme_qmap_get_tagset(qmap);
	struct request_queue *q;

	mutex_lock(&set->tag_list_lock);
	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		restore_driver_data(set, q);
	}
	mutex_unlock(&set->tag_list_lock);
}

static void nvme_qmap_zero_sets(struct nvme_qmap *qmap)
{
	memset(&qmap->last_sets, 0, sizeof(qmap->last_sets));
}

/* works only in nvme reset. no need to sync */
void nvme_qmap_reset(struct nvme_ctrl *ctrl)
{
	int instance;
	struct nvme_qmap *qmap;
	struct blk_mq_tag_set *set;

	instance = ctrl->instance;

	if (nvme_qmap_mgr_exceed_max(instance) ||
	    !nvme_qmap_mgr_enabled(instance))
		return;

	qmap = nvme_qmap_mgr_get_by_instance(instance);

	/* maybe no tagset, such as no io queues */
	set = nvme_qmap_get_tagset(qmap);
	if (!set) {
		nvme_qmap_zero_sets(qmap);
		return;
	}

	if (!nvme_qmap_sets_same(qmap, &qmap->last_sets)) {
		qmap_warn(qmap, "sets changed. back to default.");
		nvme_qmap_init_default_map(qmap);
		nvme_qmap_replace_blk_mq_ops(qmap);
		return;
	}

	qmap_warn(qmap, "ctrl reset! restore map.");
	nvme_qmap_replace_blk_mq_ops(qmap);
	blk_mq_update_nr_hw_queues(set, qmap->sets.nr_blk_mq);
	nvme_qmap_update_driver_data(set);
}
EXPORT_SYMBOL_GPL(nvme_qmap_reset);

void nvme_qmap_add_enable_attr(struct nvme_ctrl *ctrl, const struct attribute *attr)
{
	if (nvme_qmap_mgr_exceed_max(ctrl->instance)) {
		dev_warn(ctrl->device, "qmap:instance exceed max.");
		return;
	}

	if (nvme_qmap_mgr_get_attr(ctrl->instance))
		return;

	if (sysfs_add_file_to_group(&ctrl->device->kobj, attr, NULL)) {
		dev_err(ctrl->device, "qmap:qmap enable attr failed to add");
		return;
	}
	nvme_qmap_mgr_set_attr(ctrl->instance, attr);
}
EXPORT_SYMBOL_GPL(nvme_qmap_add_enable_attr);

void nvme_qmap_remove_enable_attr(struct nvme_ctrl *ctrl)
{
	const struct attribute *attr;

	if (nvme_qmap_mgr_exceed_max(ctrl->instance))
		return;

	attr = nvme_qmap_mgr_get_attr(ctrl->instance);
	if (!attr)
		return;

	sysfs_remove_file_from_group(&ctrl->device->kobj, attr, NULL);
	nvme_qmap_mgr_set_attr(ctrl->instance, NULL);
}
EXPORT_SYMBOL_GPL(nvme_qmap_remove_enable_attr);

static void nvme_qmap_do_restore(struct nvme_ctrl *ctrl)
{
	struct nvme_qmap *qmap;

	qmap = nvme_qmap_mgr_get_by_instance(ctrl->instance);

	/* maybe no tagset, such as no io queues */
	if (!nvme_qmap_get_tagset(qmap))
		return;
	nvme_qmap_restore_tagset(qmap);
	nvme_qmap_restore_blk_mq_ops(qmap);
}

/* sync with enable and restore to make original routine works fine */
void nvme_qmap_restore(struct nvme_ctrl *ctrl)
{
	if (nvme_qmap_mgr_exceed_max(ctrl->instance) ||
	    !nvme_qmap_mgr_enabled(ctrl->instance))
		return;

	/* sync nvme_reset with qmap routine */
	mutex_lock(get_lock(ctrl));
	nvme_qmap_do_restore(ctrl);
	mutex_unlock(get_lock(ctrl));
}
EXPORT_SYMBOL_GPL(nvme_qmap_restore);

bool nvme_qmap_mgr_exceed_max(int instance)
{
	return instance >= MAX_QMAP;
}
EXPORT_SYMBOL_GPL(nvme_qmap_mgr_exceed_max);

bool nvme_qmap_mgr_enabled(int instance)
{
	return qmap_mgr[instance].comp_map;
}
EXPORT_SYMBOL_GPL(nvme_qmap_mgr_enabled);

/* check enabled before use this */
int nvme_qmap_nqid_to_mqid(int instance, int nvme_qid)
{
	return qmap_mgr[instance].comp_map[nvme_qid];
}
EXPORT_SYMBOL_GPL(nvme_qmap_nqid_to_mqid);

static void nvme_qmap_free_ctrl(struct nvme_ctrl *ctrl)
{
	struct nvme_qmap *qmap = nvme_qmap_mgr_get_by_instance(ctrl->instance);
	struct nvme_qmap_internal *qmap_inter = to_internal(qmap);
	void (*free_ctrl)(struct nvme_ctrl *ctrl) = qmap_inter->ctrl_ops->free_ctrl;

	nvme_qmap_remove_files(qmap);
	nvme_qmap_mgr_remove_qmap(qmap);
	nvme_qmap_free_qmap(qmap);
	free_ctrl(ctrl);
}

static void nvme_qmap_cache_ctrl_ops(struct nvme_qmap *qmap)
{
	struct nvme_ctrl *ctrl = qmap->ctrl;
	struct nvme_qmap_internal *qmap_inter = to_internal(qmap);
	const struct nvme_ctrl_ops *origin_ops = ctrl->ops;

	qmap_inter->ctrl_ops = origin_ops;
	memcpy(&qmap_inter->nc_ops, origin_ops, sizeof(struct nvme_ctrl_ops));
	qmap_inter->nc_ops.free_ctrl = nvme_qmap_free_ctrl;
}

static inline void nvme_qmap_replace_ctrl_ops(struct nvme_qmap *qmap)
{
	struct nvme_qmap_internal *qmap_inter = to_internal(qmap);

	qmap->ctrl->ops = &qmap_inter->nc_ops;
}

static inline void nvme_qmap_restore_ctrl_ops(struct nvme_qmap *qmap)
{
	struct nvme_qmap_internal *qmap_inter = to_internal(qmap);

	qmap->ctrl->ops = qmap_inter->ctrl_ops;
}

static int nvme_qmap_do_enable
(struct nvme_ctrl *ctrl, struct nvme_queue *nq, u64 size, unsigned int nr_allocated_queues)
{
	struct nvme_qmap *qmap = NULL;
	int ret = 0;

	qmap = nvme_qmap_alloc(ctrl, nq, size, nr_allocated_queues);
	if (!qmap)
		return -ENOMEM;

	nvme_qmap_cache_nvme_queues(qmap);
	nvme_qmap_init_default_map(qmap);
	nvme_qmap_cache_ctrl_ops(qmap);
	nvme_qmap_cache_blk_mq_ops(qmap);

	nvme_qmap_replace_ctrl_ops(qmap);
	nvme_qmap_replace_blk_mq_ops(qmap);
	nvme_qmap_mgr_add_qmap(qmap);

	if (nvme_qmap_add_files(qmap)) {
		ret = -EFAULT;
		goto err_mgr_remove;
	}

	return 0;

err_mgr_remove:
	nvme_qmap_mgr_remove_qmap(qmap);
	nvme_qmap_restore_blk_mq_ops(qmap);
	nvme_qmap_restore_ctrl_ops(qmap);
	nvme_qmap_free_qmap(qmap);

	return ret;
}

static int nvme_qmap_enable_qmap(struct nvme_ctrl *ctrl,
				 struct nvme_queue *nq,
				 u64 size,
				 unsigned int nr_allocated_queues)
{
	int ret;

	if (!ctrl->tagset) {
		dev_err(ctrl->device, "qmap:no tagset. can't enable qmap.");
		return -EINVAL;
	}

	if (nvme_qmap_mgr_enabled(ctrl->instance))
		return -EINVAL;

	ret = nvme_qmap_cease_io(ctrl);
	if (ret)
		return ret;
	ret = nvme_qmap_do_enable(ctrl, nq, size, nr_allocated_queues);
	nvme_qmap_restart_io(ctrl);

	return ret;
}

int nvme_qmap_enable_dynamically(struct nvme_ctrl *ctrl,
				 struct nvme_queue *nq,
				 u64 size,
				 unsigned int nr_allocated_queues)
{
	int ret;

	if (nvme_qmap_mgr_exceed_max(ctrl->instance)) {
		dev_err(ctrl->device, "qmap:instacne:%d excced max:%u", ctrl->instance, MAX_QMAP);
		return -EINVAL;
	}

	if (nvme_qmap_mgr_enabled(ctrl->instance))
		return -EINVAL;

	ret = nvme_qmap_wait_compl(ctrl);
	if (ret)
		return ret;

	ret = mutex_lock_interruptible(&ctrl->scan_lock);
	if (ret) {
		nvme_qmap_finish_compl(ctrl);
		return ret;
	}

	ret = nvme_qmap_enable_qmap(ctrl, nq, size, nr_allocated_queues);
	mutex_unlock(&ctrl->scan_lock);

	nvme_qmap_finish_compl(ctrl);

	return ret;
}
EXPORT_SYMBOL_GPL(nvme_qmap_enable_dynamically);

/* only in nvme_reset_work, so need to sync */
void nvme_qmap_enable_at_startup(struct nvme_ctrl *ctrl,
				 struct nvme_queue *nq,
				 u64 size,
				 unsigned int nr_allocated_queues)
{
	int instance = ctrl->instance;

	if (!enable_at_startup)
		return;

	if (nvme_qmap_mgr_exceed_max(instance)) {
		dev_warn(ctrl->device, "qmap:instacne:%d excced max:%u", instance, MAX_QMAP);
		return;
	}

	if (nvme_qmap_mgr_enabled(ctrl->instance))
		return;

	if (!ctrl->tagset) {
		dev_err(ctrl->device, "qmap:no tagset. can't be enabled");
		return;
	}

	if (nvme_qmap_do_enable(ctrl, nq, size, nr_allocated_queues))
		dev_err(ctrl->dev, "qmap:failed to enable qmap");
}
EXPORT_SYMBOL_GPL(nvme_qmap_enable_at_startup);

static int __init qmap_init(void)
{
	if (nvme_qmap_init_qmap_mgr())
		return -EFAULT;

	/* change enable_at_startup here */
	/* change show_comptible_interface here */

	return 0;
}

static void __exit qmap_exit(void)
{
	nvme_qmap_uninit_qmap_mgr();
}

MODULE_AUTHOR("dingtao.darren <dingtao.darren@bytedance.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");
module_init(qmap_init);
module_exit(qmap_exit);
