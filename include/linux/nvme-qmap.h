/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Definitions for the NVM Express qmap interface
 * Copyright (c) 2024, Bytedance Corporation.
 */

#ifndef NVME_QMAP_H
#define NVME_QMAP_H

#include <linux/debugfs.h>

#include <linux/blk-mq.h>
#include <linux/interrupt.h>
#include <linux/irqnr.h>

struct qidmask {
	long *bits;
	unsigned int nr;
};

enum nvme_qmap_flag {
	QMAP_ENABLED = 0,
	QMAP_DISABLED = 1,
	QMAP_OCCUPIED = 2
};

/*
 * its length equal to nvme queue
 * the index is the same as nvme io qid
 */
struct nvme_qmap_nmap {
	struct qidmask qid_mask;	/* which queue forword to me */
	int flag_disabled;		/* disabled or not */
	int dst_qid;			/* which nvme queue I forword */
};

/*
 * stores the blk_mq map to nvme queue
 * use it to directly map blk mq
 */
struct nvme_qmap_mmap {
	cpumask_var_t mask;	/* cpu mask */
	int nvme_qid;		/* directly find nvmeq */
};

/* index is same as nvme io qid:
 * map info for cq -> blk_mq
 * completion queue map
 */
struct nvme_qmap_cmap {
	int dst_mqid;
};

struct nvme_qmap_set {
	unsigned int nr_ques[HCTX_MAX_TYPES];
	u32 nr_blk_mq;		/* stores the current blk mq queues */
};

/* compiler will help checking */
struct nvme_dev;
struct nvme_ctrl;

struct nvme_qmap {
	struct list_head qmap_entry;
	struct nvme_qmap_nmap *nvme_map;
	struct nvme_qmap_nmap *shadow_nvme_map;
	struct nvme_qmap_mmap *blk_mq_map;
	u16  *comp_map;		/* map info for cq -> blk_mq */
	struct nvme_qmap_set sets;
	struct nvme_qmap_set shadow_sets;
	struct nvme_qmap_set last_sets;
	struct nvme_queue **nvmeq;
	struct nvme_ctrl *ctrl;
	struct nvme_dev *nvme_dev;
	int nr_queues;		/* only used in free */
	bool sysfs_added;
#ifdef CONFIG_DEBUG_FS
	struct dentry *dbg_file;
#endif
};

struct nvme_qmap_qid_param {
	int (*map_arr)[2];
	int nr_groups;
};

bool nvme_qmap_mgr_enabled(int instance);
bool nvme_qmap_mgr_exceed_max(int instance);
struct nvme_qmap *nvme_qmap_mgr_get_by_instance(int instance);
int nvme_qmap_wait_compl(struct nvme_ctrl *ctrl);
void nvme_qmap_finish_compl(struct nvme_ctrl *ctrl);
struct nvme_qmap *nvme_qmap_get_qmap_from_ndev(struct nvme_dev *ndev);
void nvme_qmap_restore(struct nvme_ctrl *ctrl);
void nvme_qmap_reset(struct nvme_ctrl *ctrl);
int nvme_qmap_enable_dynamically
(struct nvme_ctrl *ctrl, struct nvme_queue *nq, u64 size, unsigned int nr_allocated_queues);
int nvme_qmap_nqid_to_mqid(int instance, int nvme_qid);
void nvme_qmap_enable_at_startup
(struct nvme_ctrl *ctrl, struct nvme_queue *nq, u64 size, unsigned int nr_allocated_queues);
void nvme_qmap_add_enable_attr(struct nvme_ctrl *ctrl, const struct attribute *attr);
void nvme_qmap_remove_enable_attr(struct nvme_ctrl *ctrl);
int nvme_qmap_map_queues(struct nvme_ctrl *ctrl, struct nvme_qmap_qid_param *param);

/*
 * iterate all qid mask
 * attention qid must be int
 */
#define for_each_qid(qid, maskp)						\
	for ((qid) = -1;							\
		(qid) = find_next_bit((maskp)->bits, (maskp)->nr, (qid) + 1),	\
		(qid) < (maskp)->nr;)

static inline void qidmask_clear(struct qidmask *msk)
{
	bitmap_zero(msk->bits, msk->nr);
}

static inline void qidmask_set_qid(unsigned int qid, struct qidmask *msk)
{
	set_bit(qid, msk->bits);
}

static inline bool zalloc_qidmask(struct qidmask *msk, unsigned int nr)
{
	int size = BITS_TO_LONGS(nr) * sizeof(long);

	msk->nr = nr;
	msk->bits = kmalloc_node(size,
				 __GFP_ZERO | GFP_KERNEL, NUMA_NO_NODE);
	return !!msk->bits;
}

static inline void free_qidmask(struct qidmask *msk)
{
	kfree(msk->bits);
}

static inline void qidmask_copy(struct qidmask *dst, struct qidmask *src)
{
	bitmap_copy(dst->bits, src->bits, src->nr);
}

static inline void qidmask_clear_qid(int qid, struct qidmask *msk)
{
	clear_bit(qid, msk->bits);
}

/* qid is exactly the same as nvme io qid */
#define QMAP_FUNC_FLAG_STATUS(name, flag) \
static inline bool nvme_qmap_qid_flag_##name(struct nvme_qmap *qmap, int qid) \
{ \
	struct nvme_qmap_nmap *nmap = &qmap->nvme_map[qid]; \
	return nmap->flag_disabled == (flag); \
}

QMAP_FUNC_FLAG_STATUS(occupied, QMAP_OCCUPIED)
QMAP_FUNC_FLAG_STATUS(disabled, QMAP_DISABLED)
QMAP_FUNC_FLAG_STATUS(enabled, QMAP_ENABLED)

#define QMAP_FUNC_FLAG_SET(name, flag) \
static inline void nvme_qmap_qid_flag_set_##name(struct nvme_qmap *qmap, int qid) \
{ \
	qmap->nvme_map[qid].flag_disabled = (flag); \
	qmap->shadow_nvme_map[qid].flag_disabled = (flag); \
}

QMAP_FUNC_FLAG_SET(occupied, QMAP_OCCUPIED)
QMAP_FUNC_FLAG_SET(disabled, QMAP_DISABLED)
QMAP_FUNC_FLAG_SET(enabled, QMAP_ENABLED)
#endif /* NVME_QMAP_H */
