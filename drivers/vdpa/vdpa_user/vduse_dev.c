// SPDX-License-Identifier: GPL-2.0-only
/*
 * VDUSE: vDPA Device in Userspace
 *
 * Copyright (C) 2020-2021 Bytedance Inc. and/or its affiliates. All rights reserved.
 *
 * Author: Xie Yongji <xieyongji@bytedance.com>
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>
#include <linux/poll.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/vdpa.h>
#include <linux/nospec.h>
#include <linux/vringh.h>
#include <linux/vmalloc.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/interrupt.h>
#include <uapi/linux/vduse.h>
#include <uapi/linux/vdpa.h>
#include <uapi/linux/virtio_config.h>
#include <uapi/linux/virtio_blk.h>
#include <uapi/linux/fuse.h>
#include <linux/mod_devicetable.h>

#include "iova_domain.h"

#define DRV_VERSION  "1.0"
#define DRV_AUTHOR   "Yongji Xie <xieyongji@bytedance.com>"
#define DRV_DESC     "vDPA Device in Userspace"
#define DRV_LICENSE  "GPL v2"

#define VDUSE_DEV_MAX (1U << MINORBITS)
#define VDUSE_REQUEST_TIMEOUT 30

#define MIN_CUSTOM_VIRTIO_ID 48

struct vduse_dev;

struct vduse_virtqueue {
	u16 index;
	bool ready;
	spinlock_t kick_lock;
	struct rw_semaphore irq_lock;
	struct eventfd_ctx *kickfd;
	struct vdpa_callback cb;
	struct work_struct inject;
	struct kobject kobj;
	int irq_effective_cpu;
	struct cpumask affinity;
	bool automatic_affinity;
	bool irq_use_wq;
	u16 avail_index;
	u32 num;
	u64 desc_addr;
	u64 device_addr;
	u64 driver_addr;
	struct vringh vring;
	struct vringh_kiov in_iov;
	struct vringh_kiov out_iov;
	struct vduse_dev *dev;
};

struct vduse_vdpa {
	struct vdpa_device vdpa;
	struct list_head vdpa_list;
	struct vduse_dev *dev;
	struct mutex lock;
};

struct vduse_umem {
	unsigned long iova;
	unsigned long npages;
	struct page **pages;
	struct mm_struct *mm;
};

struct vduse_dev {
	struct vduse_vdpa *vdev;
	struct device dev;
	struct cdev cdev;
	struct vduse_virtqueue **vqs;
	struct vduse_iova_domain *domain;
	char *name;
	struct mutex lock;
	spinlock_t msg_lock;
	atomic64_t msg_unique;
	wait_queue_head_t waitq;
	struct list_head send_list;
	struct list_head recv_list;
	struct list_head list;
	struct vdpa_callback config_cb;
	struct work_struct inject;
	spinlock_t irq_lock;
	unsigned long api_version;
	bool connected;
	bool aborted;
	int minor;
	u16 req_cached;
	u16 vq_size_max;
	u32 vq_num;
	u32 vq_align;
	u32 config_size;
	u32 device_id;
	u32 vendor_id;
	u64 device_features;
	u64 driver_features;
	u8 status;
	struct delayed_work timeout_work;
	u16 dead_timeout;
	int (*dead_handler)(struct vduse_dev *dev, struct vduse_virtqueue *vq);
	void *shm_addr;
	u8 dev_shm_size;
	u8 vq_shm_off;
	bool dead;
	bool hung;
	spinlock_t config_lock;
	void *config;
	struct vduse_umem *umem;
	struct mutex mem_lock;
};

struct vduse_dev_msg {
	struct vduse_dev_request req;
	struct vduse_dev_response resp;
	struct list_head list;
	wait_queue_head_t waitq;
	bool completed;
};

struct vduse_control {
	unsigned long api_version;
};

static bool allow_unsafe_device;
module_param(allow_unsafe_device, bool, 0644);
MODULE_PARM_DESC(allow_unsage_device, "Allow emulating unsafe device types");

static unsigned long max_bounce_size = (1024 * 1024 * 1024UL);
module_param(max_bounce_size, ulong, 0444);
MODULE_PARM_DESC(max_bounce_size, "Maximum bounce buffer size. (default: 1G)");

static unsigned long max_iova_size = (2048 * 1024 * 1024UL);
module_param(max_iova_size, ulong, 0444);
MODULE_PARM_DESC(max_iova_size, "Maximum iova space size (default: 2G)");

static DEFINE_MUTEX(vduse_lock);
static LIST_HEAD(vduse_devs);
static DEFINE_IDA(vduse_ida);
static LIST_HEAD(vdpa_devs);

static dev_t vduse_major;
static struct class *vduse_class;
static struct workqueue_struct *vduse_irq_wq;
static struct workqueue_struct *vduse_irq_bound_wq;

static u32 allowed_device_id[] = {
	VIRTIO_ID_BLOCK,
	VIRTIO_ID_FS,
	VIRTIO_ID_NET,
};

static inline struct vduse_dev *vdpa_to_vduse(struct vdpa_device *vdpa)
{
	struct vduse_vdpa *vdev = container_of(vdpa, struct vduse_vdpa, vdpa);

	return vdev->dev;
}

static inline struct vduse_dev *dev_to_vduse(struct device *dev)
{
	struct vdpa_device *vdpa = dev_to_vdpa(dev);

	return vdpa_to_vduse(vdpa);
}

static struct vduse_dev_msg *vduse_find_msg(struct list_head *head,
					    uint32_t request_id)
{
	struct vduse_dev_msg *tmp, *msg = NULL;

	list_for_each_entry(tmp, head, list) {
		if (tmp->req.request_id == request_id) {
			msg = tmp;
			list_del(&tmp->list);
			break;
		}
	}

	return msg;
}

static struct vduse_dev_msg *vduse_dequeue_msg(struct list_head *head)
{
	struct vduse_dev_msg *msg = NULL;

	if (!list_empty(head)) {
		msg = list_first_entry(head, struct vduse_dev_msg, list);
		list_del(&msg->list);
	}

	return msg;
}

static void vduse_enqueue_msg(struct list_head *head,
			      struct vduse_dev_msg *msg)
{
	list_add_tail(&msg->list, head);
}

static int vduse_dev_msg_sync(struct vduse_dev *dev,
			      struct vduse_dev_msg *msg)
{
	long timeout = dev->dead ? 2 : VDUSE_REQUEST_TIMEOUT;

	init_waitqueue_head(&msg->waitq);
	spin_lock(&dev->msg_lock);
	if (dev->dead && (!dev->connected || dev->hung)) {
		msg->resp.result = VDUSE_REQUEST_FAILED;
		goto unlock;
	}

	vduse_enqueue_msg(&dev->send_list, msg);
	wake_up(&dev->waitq);
	spin_unlock(&dev->msg_lock);
	wait_event_killable_timeout(msg->waitq, msg->completed,
				    timeout * HZ);
	spin_lock(&dev->msg_lock);
	if (!msg->completed) {
		if (dev->dead)
			dev->hung = true;
		list_del(&msg->list);
		msg->resp.result = VDUSE_REQUEST_FAILED;
	}
unlock:
	spin_unlock(&dev->msg_lock);
	if (!dev->dead && msg->resp.result != VDUSE_REQUEST_OK)
		WARN(1, "vduse message (type: %d) failed\n", msg->req.type);

	return (msg->resp.result == VDUSE_REQUEST_OK) ? 0 : -1;
}

static u32 vduse_dev_get_request_id(struct vduse_dev *dev)
{
	return atomic64_fetch_inc(&dev->msg_unique);
}

static u64 vduse_dev_get_features(struct vduse_dev *dev)
{
	struct vduse_dev_msg msg = {{ 0 }};

	msg.req.type = VDUSE_GET_FEATURES;
	msg.req.request_id = vduse_dev_get_request_id(dev);

	if (vduse_dev_msg_sync(dev, &msg) != 0)
		return 0;

	return msg.resp.f.features;
}

static int vduse_dev_set_features(struct vduse_dev *dev, u64 features)
{
	struct vduse_dev_msg msg = {{ 0 }};

	msg.req.type = VDUSE_SET_FEATURES;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.f.features = features;

	return vduse_dev_msg_sync(dev, &msg);
}

static u8 vduse_dev_get_status(struct vduse_dev *dev)
{
	struct vduse_dev_msg msg = {{ 0 }};

	msg.req.type = VDUSE_GET_STATUS;
	msg.req.request_id = vduse_dev_get_request_id(dev);

	if (vduse_dev_msg_sync(dev, &msg) != 0)
		return 0;

	return msg.resp.s.status;
}

static void vduse_dev_set_status(struct vduse_dev *dev, u8 status)
{
	struct vduse_dev_msg msg = {{ 0 }};

	msg.req.type = VDUSE_SET_STATUS;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.s.status = status;

	vduse_dev_msg_sync(dev, &msg);
}

static void vduse_dev_get_config(struct vduse_dev *dev, unsigned int offset,
				 void *buf, unsigned int len)
{
	struct vduse_dev_msg msg = {{ 0 }};
	unsigned int sz;

	while (len) {
		sz = min_t(unsigned int, len, sizeof(msg.req.config.data));
		msg.req.type = VDUSE_GET_CONFIG;
		msg.req.request_id = vduse_dev_get_request_id(dev);
		msg.req.config.offset = offset;
		msg.req.config.len = sz;
		if (vduse_dev_msg_sync(dev, &msg) != 0)
			break;

		memcpy(buf, msg.resp.config.data, sz);
		buf += sz;
		offset += sz;
		len -= sz;
	}
}

static void vduse_dev_set_config(struct vduse_dev *dev, unsigned int offset,
				 const void *buf, unsigned int len)
{
	struct vduse_dev_msg msg = {{ 0 }};
	unsigned int sz;

	while (len) {
		sz = min_t(unsigned int, len, sizeof(msg.req.config.data));
		msg.req.type = VDUSE_SET_CONFIG;
		msg.req.request_id = vduse_dev_get_request_id(dev);
		msg.req.config.offset = offset;
		msg.req.config.len = sz;
		memcpy(msg.req.config.data, buf, sz);
		if (vduse_dev_msg_sync(dev, &msg) != 0)
			break;

		buf += sz;
		offset += sz;
		len -= sz;
	}
}

static void vduse_dev_set_vq_num(struct vduse_dev *dev,
				 struct vduse_virtqueue *vq, u32 num)
{
	struct vduse_dev_msg msg = {{ 0 }};

	msg.req.type = VDUSE_SET_VQ_NUM;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.vq_num.index = vq->index;
	msg.req.vq_num.num = num;

	vduse_dev_msg_sync(dev, &msg);
}

static int vduse_dev_set_vq_addr(struct vduse_dev *dev,
				 struct vduse_virtqueue *vq, u64 desc_addr,
				 u64 driver_addr, u64 device_addr)
{
	struct vduse_dev_msg msg = {{ 0 }};

	msg.req.type = VDUSE_SET_VQ_ADDR;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.vq_addr.index = vq->index;
	msg.req.vq_addr.desc_addr = desc_addr;
	msg.req.vq_addr.driver_addr = driver_addr;
	msg.req.vq_addr.device_addr = device_addr;

	return vduse_dev_msg_sync(dev, &msg);
}

static void vduse_dev_set_vq_ready(struct vduse_dev *dev,
				struct vduse_virtqueue *vq, bool ready)
{
	struct vduse_dev_msg msg = {{ 0 }};

	msg.req.type = VDUSE_SET_VQ_READY;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.vq_ready.index = vq->index;
	msg.req.vq_ready.ready = ready;

	vduse_dev_msg_sync(dev, &msg);
}

static bool vduse_dev_get_vq_ready(struct vduse_dev *dev,
				   struct vduse_virtqueue *vq)
{
	struct vduse_dev_msg msg = {{ 0 }};

	msg.req.type = VDUSE_GET_VQ_READY;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.vq_ready.index = vq->index;
	if (vduse_dev_msg_sync(dev, &msg))
		return false;

	return msg.resp.vq_ready.ready;
}

static int vduse_dev_get_vq_state(struct vduse_dev *dev,
				struct vduse_virtqueue *vq,
				struct vdpa_vq_state *state)
{
	struct vduse_dev_msg msg = {{ 0 }};
	int ret;

	msg.req.type = VDUSE_GET_VQ_STATE;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.vq_state.index = vq->index;

	ret = vduse_dev_msg_sync(dev, &msg);
	if (!ret)
		state->avail_index = msg.resp.vq_state.avail_idx;

	return ret;
}

static int vduse_dev_set_vq_state(struct vduse_dev *dev,
				struct vduse_virtqueue *vq,
				const struct vdpa_vq_state *state)
{
	struct vduse_dev_msg msg = {{ 0 }};

	msg.req.type = VDUSE_SET_VQ_STATE;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.vq_state.index = vq->index;
	msg.req.vq_state.avail_idx = state->avail_index;

	return vduse_dev_msg_sync(dev, &msg);
}

static int vduse_dev_update_iotlb(struct vduse_dev *dev,
				u64 start, u64 last)
{
	struct vduse_dev_msg msg = {{ 0 }};

	if (last < start)
		return -EINVAL;

	msg.req.type = VDUSE_UPDATE_IOTLB;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.iova.start = start;
	msg.req.iova.last = last;

	return vduse_dev_msg_sync(dev, &msg);
}

static int vduse_dev_vdpa_disconnect(struct vduse_dev *dev)
{
	struct vduse_dev_msg msg = {{ 0 }};

	msg.req.type = VDUSE_VDPA_DISCONNECT;
	msg.req.request_id = vduse_dev_get_request_id(dev);

	return vduse_dev_msg_sync(dev, &msg);
}

static ssize_t vduse_dev_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct vduse_dev *dev = file->private_data;
	struct vduse_dev_msg *msg;
	int size = sizeof(struct vduse_dev_request);
	ssize_t ret;

	if (iov_iter_count(to) < size)
		return -EINVAL;

	spin_lock(&dev->msg_lock);
	while (1) {
		msg = vduse_dequeue_msg(&dev->send_list);
		if (msg)
			break;

		ret = -EAGAIN;
		if (file->f_flags & O_NONBLOCK)
			goto unlock;

		spin_unlock(&dev->msg_lock);
		ret = wait_event_interruptible_exclusive(dev->waitq,
					!list_empty(&dev->send_list));
		if (ret)
			return ret;

		spin_lock(&dev->msg_lock);
	}
	spin_unlock(&dev->msg_lock);
	ret = copy_to_iter(&msg->req, size, to);
	spin_lock(&dev->msg_lock);
	if (ret != size) {
		ret = -EFAULT;
		vduse_enqueue_msg(&dev->send_list, msg);
		goto unlock;
	}
	vduse_enqueue_msg(&dev->recv_list, msg);
unlock:
	spin_unlock(&dev->msg_lock);

	return ret;
}

static bool is_mem_zero(const char *ptr, int size)
{
	int i;

	for (i = 0; i < size; i++) {
		if (ptr[i])
			return false;
	}
	return true;
}

static ssize_t vduse_dev_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct vduse_dev *dev = file->private_data;
	struct vduse_dev_response resp;
	struct vduse_dev_msg *msg;
	size_t ret;

	ret = copy_from_iter(&resp, sizeof(resp), from);
	if (ret != sizeof(resp))
		return -EINVAL;

	spin_lock(&dev->msg_lock);
	msg = vduse_find_msg(&dev->recv_list, resp.request_id);
	if (!msg) {
		ret = -ENOENT;
		goto unlock;
	}

	memcpy(&msg->resp, &resp, sizeof(resp));
	msg->completed = 1;
	wake_up(&msg->waitq);
unlock:
	spin_unlock(&dev->msg_lock);

	return ret;
}

static __poll_t vduse_dev_poll(struct file *file, poll_table *wait)
{
	struct vduse_dev *dev = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &dev->waitq, wait);

	if (!list_empty(&dev->send_list))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (!list_empty(&dev->recv_list))
		mask |= EPOLLOUT | EPOLLWRNORM;

	return mask;
}

static void vduse_dev_reset(struct vduse_dev *dev)
{
	int i;
	struct vduse_iova_domain *domain = dev->domain;

	/* The coherent mappings are handled in vduse_dev_free_coherent() */
	if (domain->bounce_map) {
		vduse_domain_reset_bounce_map(domain);
		vduse_dev_update_iotlb(dev, 0ULL, domain->bounce_size - 1);
	}

	dev->status = 0;
	dev->driver_features = 0;
	dev->device_features = 0;
	spin_lock(&dev->irq_lock);
	dev->config_cb.callback = NULL;
	dev->config_cb.private = NULL;
	spin_unlock(&dev->irq_lock);

	spin_lock(&dev->config_lock);
	kfree(dev->config);
	dev->config = NULL;
	spin_unlock(&dev->config_lock);

	for (i = 0; i < dev->vq_num; i++) {
		struct vduse_virtqueue *vq = dev->vqs[i];

		vq->desc_addr = 0;
		vq->driver_addr = 0;
		vq->device_addr = 0;
		vq->avail_index = 0;
		vq->num = 0;
		down_write(&vq->irq_lock);
		vq->ready = false;
		vq->cb.callback = NULL;
		vq->cb.private = NULL;
		up_write(&vq->irq_lock);
	}
}

static inline bool vduse_dev_req_cached(struct vduse_dev *dev, int req)
{
	return !!(dev->req_cached & (1 << req));
}

static int vduse_vdpa_set_vq_address(struct vdpa_device *vdpa, u16 idx,
				u64 desc_area, u64 driver_area,
				u64 device_area)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	vq->desc_addr = desc_area;
	vq->driver_addr = driver_area;
	vq->device_addr = device_area;

	if (!vduse_dev_req_cached(dev, VDUSE_SET_VQ_ADDR))
		return vduse_dev_set_vq_addr(dev, vq, desc_area,
					     driver_area, device_area);

	return 0;
}

static void vduse_vdpa_kick_vq(struct vdpa_device *vdpa, u16 idx)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	spin_lock(&vq->kick_lock);
	if (!vq->ready)
		goto unlock;

	if (dev->dead)
		schedule_delayed_work(&dev->timeout_work, 0);
	else if (vq->kickfd)
		eventfd_signal(vq->kickfd, 1);
unlock:
	spin_unlock(&vq->kick_lock);
}

static void vduse_vdpa_set_vq_cb(struct vdpa_device *vdpa, u16 idx,
			      struct vdpa_callback *cb)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	down_write(&vq->irq_lock);
	vq->cb.callback = cb->callback;
	vq->cb.private = cb->private;
	up_write(&vq->irq_lock);
}

static void vduse_vdpa_set_vq_num(struct vdpa_device *vdpa, u16 idx, u32 num)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	vq->num = num;
	if (!vduse_dev_req_cached(dev, VDUSE_SET_VQ_NUM))
		vduse_dev_set_vq_num(dev, vq, num);
}

static void vduse_vdpa_set_vq_ready(struct vdpa_device *vdpa,
					u16 idx, bool ready)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	if (!vduse_dev_req_cached(dev, VDUSE_SET_VQ_READY))
		vduse_dev_set_vq_ready(dev, vq, ready);

	mutex_lock(&dev->lock);
	if (ready)
		vringh_init_iotlb(&vq->vring, dev->driver_features,
			vq->num, false,
			(struct vring_desc *)(uintptr_t)vq->desc_addr,
			(struct vring_avail *)(uintptr_t)vq->driver_addr,
			(struct vring_used *)(uintptr_t)vq->device_addr);
	vq->ready = ready;
	mutex_unlock(&dev->lock);
}

static bool vduse_vdpa_get_vq_ready(struct vdpa_device *vdpa, u16 idx)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	if (!vduse_dev_req_cached(dev, VDUSE_GET_VQ_READY))
		vq->ready = vduse_dev_get_vq_ready(dev, vq);

	return vq->ready;
}

static int vduse_vdpa_set_vq_state(struct vdpa_device *vdpa, u16 idx,
				const struct vdpa_vq_state *state)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	if (!vduse_dev_req_cached(dev, VDUSE_SET_VQ_STATE))
		return vduse_dev_set_vq_state(dev, vq, state);

	vq->avail_index = state->avail_index;

	return 0;
}

static int vduse_vdpa_get_vq_state(struct vdpa_device *vdpa, u16 idx,
				struct vdpa_vq_state *state)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	return vduse_dev_get_vq_state(dev, vq, state);
}

static u32 vduse_vdpa_get_vq_align(struct vdpa_device *vdpa)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	return dev->vq_align;
}

static u64 vduse_vdpa_get_features(struct vdpa_device *vdpa)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	if (!dev->device_features ||
	    !vduse_dev_req_cached(dev, VDUSE_GET_FEATURES))
		dev->device_features = vduse_dev_get_features(dev);

	return dev->device_features;
}

static int vduse_vdpa_set_features(struct vdpa_device *vdpa, u64 features)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	if (!(features & (1ULL << VIRTIO_F_ACCESS_PLATFORM)))
		return -EINVAL;

	dev->driver_features = features;

	if (!vduse_dev_req_cached(dev, VDUSE_SET_FEATURES))
		return vduse_dev_set_features(dev, features);

	return 0;
}

static void vduse_vdpa_set_config_cb(struct vdpa_device *vdpa,
				  struct vdpa_callback *cb)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	spin_lock(&dev->irq_lock);
	dev->config_cb.callback = cb->callback;
	dev->config_cb.private = cb->private;
	spin_unlock(&dev->irq_lock);
}

static u16 vduse_vdpa_get_vq_num_max(struct vdpa_device *vdpa)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	return dev->vq_size_max;
}

static u32 vduse_vdpa_get_device_id(struct vdpa_device *vdpa)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	return dev->device_id;
}

static u32 vduse_vdpa_get_vendor_id(struct vdpa_device *vdpa)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	return dev->vendor_id;
}

static u8 vduse_vdpa_get_status(struct vdpa_device *vdpa)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	if (!vduse_dev_req_cached(dev, VDUSE_GET_STATUS))
		dev->status = vduse_dev_get_status(dev);

	return dev->status;
}

static void vduse_vdpa_set_status(struct vdpa_device *vdpa, u8 status)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	vduse_dev_set_status(dev, status);

	dev->status = status;
	if (status == 0)
		vduse_dev_reset(dev);
}

static size_t vduse_vdpa_get_config_size(struct vdpa_device *vdpa)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	return dev->config_size;
}

static void vduse_vdpa_get_config(struct vdpa_device *vdpa, unsigned int offset,
			     void *buf, unsigned int len)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	void *config = NULL;

	if (!dev->config && dev->config_size &&
	    vduse_dev_req_cached(dev, VDUSE_GET_CONFIG)) {
		config = kmalloc(dev->config_size, GFP_KERNEL);
		if (config)
			vduse_dev_get_config(dev, 0, config,
					     dev->config_size);
	}

	/* Initialize the buffer in case of partial copy. */
	memset(buf, 0, len);

	spin_lock(&dev->config_lock);
	if (!dev->config)
		dev->config = config;
	else
		kfree(config);

	if (dev->config)
		memcpy(buf, dev->config + offset, len);
	spin_unlock(&dev->config_lock);

	if (!dev->config)
		vduse_dev_get_config(dev, offset, buf, len);
}

static void vduse_vdpa_set_config(struct vdpa_device *vdpa, unsigned int offset,
			const void *buf, unsigned int len)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	spin_lock(&dev->config_lock);
	kfree(dev->config);
	dev->config = NULL;
	spin_lock(&dev->config_lock);
	vduse_dev_set_config(dev, offset, buf, len);
}

static int vduse_vdpa_set_vq_affinity(struct vdpa_device *vdpa, u16 idx,
				      const struct cpumask *cpu_mask)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	cpumask_copy(&dev->vqs[idx]->affinity, cpu_mask);
	return 0;
}

static const struct cpumask *
vduse_vdpa_get_vq_affinity(struct vdpa_device *vdpa, u16 idx)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	return &dev->vqs[idx]->affinity;
}

static void vduse_vdpa_set_irq_affinity(struct vdpa_device *vdpa,
					struct irq_affinity *desc)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct irq_affinity_desc *affd;
	int i;

	affd = irq_create_affinity_masks(dev->vq_num, desc);
	if (!affd)
		return;

	for (i = 0; i < dev->vq_num; i++) {
		if (!dev->vqs[i]->automatic_affinity)
			continue;

		cpumask_copy(&dev->vqs[i]->affinity, &affd[i].mask);
	}
	kfree(affd);
}

static int vduse_vdpa_set_map(struct vdpa_device *vdpa,
				struct vhost_iotlb *iotlb)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	int ret;

	ret = vduse_domain_set_map(dev->domain, iotlb);
	vduse_dev_update_iotlb(dev, 0ULL, ULLONG_MAX);

	return ret;
}

static int vduse_destroy_dev(char *name);

static void vduse_vdpa_free(struct vdpa_device *vdpa)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	if (dev->connected)
		vduse_dev_vdpa_disconnect(dev);

	WARN_ON(!list_empty(&dev->send_list));
	WARN_ON(!list_empty(&dev->recv_list));
	dev->vdev = NULL;
	if (dev->dead && !dev->connected) {
		pr_info("VDUSE: destroy dead device: %s\n", dev->name);
		vduse_destroy_dev(dev->name);
	}
}

static const struct vdpa_config_ops vduse_vdpa_config_ops = {
	.set_vq_address		= vduse_vdpa_set_vq_address,
	.kick_vq		= vduse_vdpa_kick_vq,
	.set_vq_cb		= vduse_vdpa_set_vq_cb,
	.set_vq_num             = vduse_vdpa_set_vq_num,
	.set_vq_ready		= vduse_vdpa_set_vq_ready,
	.get_vq_ready		= vduse_vdpa_get_vq_ready,
	.set_vq_state		= vduse_vdpa_set_vq_state,
	.get_vq_state		= vduse_vdpa_get_vq_state,
	.get_vq_align		= vduse_vdpa_get_vq_align,
	.get_features		= vduse_vdpa_get_features,
	.set_features		= vduse_vdpa_set_features,
	.set_config_cb		= vduse_vdpa_set_config_cb,
	.get_vq_num_max		= vduse_vdpa_get_vq_num_max,
	.get_device_id		= vduse_vdpa_get_device_id,
	.get_vendor_id		= vduse_vdpa_get_vendor_id,
	.get_status		= vduse_vdpa_get_status,
	.set_status		= vduse_vdpa_set_status,
	.get_config_size	= vduse_vdpa_get_config_size,
	.get_config		= vduse_vdpa_get_config,
	.set_config		= vduse_vdpa_set_config,
	.set_vq_affinity        = vduse_vdpa_set_vq_affinity,
	.get_vq_affinity        = vduse_vdpa_get_vq_affinity,
	.set_irq_affinity       = vduse_vdpa_set_irq_affinity,
	.set_map		= vduse_vdpa_set_map,
	.free			= vduse_vdpa_free,
};

static dma_addr_t vduse_dev_map_page(struct device *dev, struct page *page,
				     unsigned long offset, size_t size,
				     enum dma_data_direction dir,
				     unsigned long attrs)
{
	struct vduse_dev *vdev = dev_to_vduse(dev);
	struct vduse_iova_domain *domain = vdev->domain;

	return vduse_domain_map_page(domain, page, offset, size, dir, attrs);
}

static void vduse_dev_unmap_page(struct device *dev, dma_addr_t dma_addr,
				size_t size, enum dma_data_direction dir,
				unsigned long attrs)
{
	struct vduse_dev *vdev = dev_to_vduse(dev);
	struct vduse_iova_domain *domain = vdev->domain;

	return vduse_domain_unmap_page(domain, dma_addr, size, dir, attrs);
}

static void *vduse_dev_alloc_coherent(struct device *dev, size_t size,
					dma_addr_t *dma_addr, gfp_t flag,
					unsigned long attrs)
{
	struct vduse_dev *vdev = dev_to_vduse(dev);
	struct vduse_iova_domain *domain = vdev->domain;
	unsigned long iova;
	void *addr;

	*dma_addr = DMA_MAPPING_ERROR;
	addr = vduse_domain_alloc_coherent(domain, size,
				(dma_addr_t *)&iova, flag, attrs);
	if (!addr)
		return NULL;

	*dma_addr = (dma_addr_t)iova;
	vduse_dev_update_iotlb(vdev, iova, iova + size - 1);

	return addr;
}

static void vduse_dev_free_coherent(struct device *dev, size_t size,
					void *vaddr, dma_addr_t dma_addr,
					unsigned long attrs)
{
	struct vduse_dev *vdev = dev_to_vduse(dev);
	struct vduse_iova_domain *domain = vdev->domain;
	unsigned long start = (unsigned long)dma_addr;
	unsigned long last = start + size - 1;

	vduse_domain_free_coherent(domain, size, vaddr, dma_addr, attrs);
	vduse_dev_update_iotlb(vdev, start, last);
}

static const struct dma_map_ops vduse_dev_dma_ops = {
	.map_page = vduse_dev_map_page,
	.unmap_page = vduse_dev_unmap_page,
	.alloc = vduse_dev_alloc_coherent,
	.free = vduse_dev_free_coherent,
};

static unsigned int perm_to_file_flags(u8 perm)
{
	unsigned int flags = 0;

	switch (perm) {
	case VDUSE_ACCESS_WO:
		flags |= O_WRONLY;
		break;
	case VDUSE_ACCESS_RO:
		flags |= O_RDONLY;
		break;
	case VDUSE_ACCESS_RW:
		flags |= O_RDWR;
		break;
	default:
		WARN(1, "invalidate vhost IOTLB permission\n");
		break;
	}

	return flags;
}

static int vduse_kickfd_setup(struct vduse_dev *dev,
			struct vduse_vq_eventfd *eventfd)
{
	struct eventfd_ctx *ctx = NULL;
	struct vduse_virtqueue *vq;
	u32 index;

	if (eventfd->index >= dev->vq_num)
		return -EINVAL;

	index = array_index_nospec(eventfd->index, dev->vq_num);
	vq = dev->vqs[index];
	if (eventfd->fd >= 0) {
		ctx = eventfd_ctx_fdget(eventfd->fd);
		if (IS_ERR(ctx))
			return PTR_ERR(ctx);
	} else if (eventfd->fd != VDUSE_EVENTFD_DEASSIGN)
		return 0;

	spin_lock(&vq->kick_lock);
	if (vq->kickfd)
		eventfd_ctx_put(vq->kickfd);
	vq->kickfd = ctx;
	spin_unlock(&vq->kick_lock);

	return 0;
}

static void vduse_dev_irq_inject(struct work_struct *work)
{
	struct vduse_dev *dev = container_of(work, struct vduse_dev, inject);
	bool missed = true;

	spin_lock_irq(&dev->irq_lock);
	if ((dev->status & VIRTIO_CONFIG_S_DRIVER_OK) &&
	    dev->config_cb.callback) {
		dev->config_cb.callback(dev->config_cb.private);
		missed = false;
	}
	spin_unlock_irq(&dev->irq_lock);
	if (missed)
		pr_err_ratelimited("Miss vduse [%s] config irq, status: %d\n",
				   dev->name, dev->status);
}

static void vduse_vq_irq_inject(struct vduse_virtqueue *vq)
{
	struct vduse_dev *dev = vq->dev;

	if (!dev)
		return;

	down_read(&vq->irq_lock);
	if ((dev->status & VIRTIO_CONFIG_S_DRIVER_OK) &&
	    vq->cb.callback)
		vq->cb.callback(vq->cb.private);
	else
		pr_err_ratelimited("Miss vduse [%s] vq%d irq, status: %d\n",
				   dev->name, vq->index, dev->status);
	up_read(&vq->irq_lock);
}

static void vduse_vq_irq_inject_work(struct work_struct *work)
{
	struct vduse_virtqueue *vq = container_of(work,
					struct vduse_virtqueue, inject);
	vduse_vq_irq_inject(vq);
}

static int vduse_dev_dereg_umem(struct vduse_dev *dev,
				u64 iova, u64 size)
{
	int ret;

	mutex_lock(&dev->mem_lock);
	ret = -ENOENT;
	if (!dev->umem)
		goto unlock;

	ret = -EINVAL;
	if (dev->umem->iova != iova || size != dev->domain->bounce_size)
		goto unlock;

	vduse_domain_remove_user_bounce_pages(dev->domain);
	unpin_user_pages_dirty_lock(dev->umem->pages,
				  dev->umem->npages, true);
	atomic64_sub(dev->umem->npages, &dev->umem->mm->pinned_vm);
	mmdrop(dev->umem->mm);
	vfree(dev->umem->pages);
	kfree(dev->umem);
	dev->umem = NULL;
	ret = 0;
unlock:
	mutex_unlock(&dev->mem_lock);
	return ret;
}

static int vduse_dev_reg_umem(struct vduse_dev *dev,
			      u64 iova, u64 uaddr, u64 size)
{
	struct page **page_list = NULL;
	struct vduse_umem *umem = NULL;
	long pinned = 0;
	unsigned long npages, lock_limit;
	int ret;

	if (!dev->domain->bounce_map ||
	    size != dev->domain->bounce_size ||
	    iova != 0 || uaddr & ~PAGE_MASK)
		return -EINVAL;

	mutex_lock(&dev->mem_lock);
	ret = -EEXIST;
	if (dev->umem)
		goto unlock;

	ret = -ENOMEM;
	npages = size >> PAGE_SHIFT;
	page_list = __vmalloc(array_size(npages, sizeof(struct page *)),
			      GFP_KERNEL_ACCOUNT);
	umem = kzalloc(sizeof(*umem), GFP_KERNEL);
	if (!page_list || !umem)
		goto unlock;

	mmap_read_lock(current->mm);

	lock_limit = PFN_DOWN(rlimit(RLIMIT_MEMLOCK));
	if (npages + atomic64_read(&current->mm->pinned_vm) > lock_limit)
		goto out;

	pinned = pin_user_pages(uaddr, npages, FOLL_LONGTERM | FOLL_WRITE,
				page_list, NULL);
	if (pinned != npages) {
		ret = pinned < 0 ? pinned : -ENOMEM;
		goto out;
	}

	ret = vduse_domain_add_user_bounce_pages(dev->domain,
						 page_list, pinned);
	if (ret)
		goto out;

	atomic64_add(npages, &current->mm->pinned_vm);

	umem->pages = page_list;
	umem->npages = pinned;
	umem->iova = iova;
	umem->mm = current->mm;
	mmgrab(current->mm);

	dev->umem = umem;
out:
	if (ret && pinned > 0)
		unpin_user_pages(page_list, pinned);

	mmap_read_unlock(current->mm);
unlock:
	if (ret) {
		vfree(page_list);
		kfree(umem);
	}
	mutex_unlock(&dev->mem_lock);
	return ret;
}

static long vduse_dev_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct vduse_dev *dev = file->private_data;
	void __user *argp = (void __user *)arg;
	int ret;

	switch (cmd) {
	case VDUSE_IOTLB_GET_FD: {
		struct vduse_iotlb_entry entry;
		struct vhost_iotlb_map *map;
		struct vdpa_map_file *map_file;
		struct vduse_iova_domain *domain = dev->domain;
		struct file *f = NULL;

		ret = -EFAULT;
		if (copy_from_user(&entry, argp, sizeof(entry)))
			break;

		ret = -EINVAL;
		if (entry.start > entry.last)
			break;

		spin_lock(&domain->iotlb_lock);
		map = vhost_iotlb_itree_first(domain->iotlb,
					      entry.start, entry.last);
		if (map) {
			map_file = (struct vdpa_map_file *)map->opaque;
			f = get_file(map_file->file);
			entry.offset = map_file->offset;
			entry.start = map->start;
			entry.last = map->last;
			entry.perm = map->perm;
		}
		spin_unlock(&domain->iotlb_lock);
		ret = -EINVAL;
		if (!f)
			break;

		ret = -EFAULT;
		if (copy_to_user(argp, &entry, sizeof(entry))) {
			fput(f);
			break;
		}
		ret = get_unused_fd_flags(perm_to_file_flags(entry.perm));
		if (ret < 0) {
			fput(f);
			break;
		}
		fd_install(ret, f);
		break;
	}
	case VDUSE_DEV_GET_FEATURES:
		/*
		 * Just mirror what driver wrote here.
		 * The driver is expected to check FEATURE_OK later.
		 */
		ret = put_user(dev->driver_features, (u64 __user *)argp);
		break;
	case VDUSE_VQ_GET_INFO: {
		struct vduse_vq_info vq_info;
		struct vduse_virtqueue *vq;
		u32 index;

		ret = -EFAULT;
		if (copy_from_user(&vq_info, argp, sizeof(vq_info)))
			break;

		ret = -EINVAL;
		if (vq_info.index >= dev->vq_num)
			break;

		index = array_index_nospec(vq_info.index, dev->vq_num);
		vq = dev->vqs[index];
		vq_info.desc_addr = vq->desc_addr;
		vq_info.driver_addr = vq->driver_addr;
		vq_info.device_addr = vq->device_addr;
		vq_info.num = vq->num;
		vq_info.split.avail_index = vq->avail_index;
		vq_info.ready = vq->ready;

		ret = -EFAULT;
		if (copy_to_user(argp, &vq_info, sizeof(vq_info)))
			break;

		ret = 0;
		break;
	}
	case VDUSE_VQ_SETUP_KICKFD: {
		struct vduse_vq_eventfd eventfd;

		ret = -EFAULT;
		if (copy_from_user(&eventfd, argp, sizeof(eventfd)))
			break;

		ret = vduse_kickfd_setup(dev, &eventfd);
		break;
	}
	case VDUSE_INJECT_VQ_IRQ: {
		u32 vq_index;
		struct vduse_virtqueue *vq;

		ret = -EFAULT;
		if (copy_from_user(&vq_index, argp, sizeof(u32)))
			break;

		ret = -EINVAL;
		if (vq_index >= dev->vq_num)
			break;

		vq_index = array_index_nospec(vq_index, dev->vq_num);
		vq = dev->vqs[vq_index];
		ret = 0;

		if (!vq->irq_use_wq) {
			vduse_vq_irq_inject(vq);
			break;
		}
		if (vq->irq_effective_cpu == -1)
			queue_work(vduse_irq_wq, &vq->inject);
		else
			queue_work_on(vq->irq_effective_cpu,
				      vduse_irq_bound_wq, &vq->inject);
		break;
	}
	case VDUSE_INJECT_CONFIG_IRQ:
		ret = 0;
		spin_lock(&dev->config_lock);
		kfree(dev->config);
		dev->config = NULL;
		spin_unlock(&dev->config_lock);
		queue_work(vduse_irq_wq, &dev->inject);
		break;
	case VDUSE_IOTLB_REG_UMEM: {
		struct vduse_iova_umem umem;

		ret = -EFAULT;
		if (copy_from_user(&umem, argp, sizeof(umem)))
			break;

		ret = -EINVAL;
		if (!is_mem_zero((const char *)umem.reserved,
				 sizeof(umem.reserved)))
			break;

		ret = vduse_dev_reg_umem(dev, umem.iova,
					 umem.uaddr, umem.size);
		break;
	}
	case VDUSE_IOTLB_DEREG_UMEM: {
		struct vduse_iova_umem umem;

		ret = -EFAULT;
		if (copy_from_user(&umem, argp, sizeof(umem)))
			break;

		ret = -EINVAL;
		if (!is_mem_zero((const char *)umem.reserved,
				 sizeof(umem.reserved)))
			break;

		ret = vduse_dev_dereg_umem(dev, umem.iova,
					   umem.size);
		break;
	}
	case VDUSE_IOTLB_GET_INFO: {
		struct vduse_iova_info info;
		struct vhost_iotlb_map *map;
		struct vduse_iova_domain *domain = dev->domain;

		ret = -EFAULT;
		if (copy_from_user(&info, argp, sizeof(info)))
			break;

		ret = -EINVAL;
		if (info.start > info.last)
			break;

		if (!is_mem_zero((const char *)info.reserved,
				 sizeof(info.reserved)))
			break;

		spin_lock(&domain->iotlb_lock);
		map = vhost_iotlb_itree_first(domain->iotlb,
					      info.start, info.last);
		if (map) {
			info.start = map->start;
			info.last = map->last;
			info.capability = 0;
			if (domain->bounce_map && map->start == 0 &&
			    map->last == domain->bounce_size - 1)
				info.capability |= VDUSE_IOVA_CAP_UMEM;
		}
		spin_unlock(&domain->iotlb_lock);
		if (!map)
			break;

		ret = -EFAULT;
		if (copy_to_user(argp, &info, sizeof(info)))
			break;

		ret = 0;
		break;
	}
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

static inline unsigned long vduse_vq_inflight_size(struct vduse_dev *dev)
{
	return ALIGN(dev->vq_shm_off + sizeof(struct vduse_vq_inflight) +
		     dev->vq_size_max * sizeof(struct desc_state_split),
		     VDUSE_SHM_ALIGNMENT);
}

static inline struct vduse_vq_inflight *
vduse_get_vq_inflight(struct vduse_dev *dev, int index)
{
	return (struct vduse_vq_inflight *)(dev->shm_addr +
		dev->dev_shm_size + dev->vq_shm_off +
		vduse_vq_inflight_size(dev) * index);
}

static int vduse_dev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vduse_dev *dev = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size < dev->dev_shm_size +
	    vduse_vq_inflight_size(dev) * dev->vq_num)
		return -EINVAL;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	if (!dev->shm_addr) {
		dev->shm_addr = vmalloc_user(size);
		if (!dev->shm_addr)
			return -ENOMEM;
	}

	return remap_vmalloc_range(vma, dev->shm_addr, 0);
}

static int vduse_dev_release(struct inode *inode, struct file *file)
{
	struct vduse_dev *dev = file->private_data;
	int i;

	for (i = 0; i < dev->vq_num; i++) {
		struct vduse_virtqueue *vq = dev->vqs[i];

		spin_lock(&vq->kick_lock);
		if (vq->kickfd)
			eventfd_ctx_put(vq->kickfd);
		vq->kickfd = NULL;
		spin_unlock(&vq->kick_lock);
	}

	vduse_dev_dereg_umem(dev, 0, dev->domain->bounce_size);
	spin_lock(&dev->msg_lock);
	/* Make sure the inflight messages can processed after reconncection */
	list_splice_init(&dev->recv_list, &dev->send_list);
	spin_unlock(&dev->msg_lock);

	dev->connected = false;
	if (dev->dead_timeout)
		schedule_delayed_work(&dev->timeout_work,
				msecs_to_jiffies(dev->dead_timeout * 1000));

	return 0;
}

static int vduse_dev_open(struct inode *inode, struct file *file)
{
	struct vduse_dev *dev = container_of(inode->i_cdev,
					struct vduse_dev, cdev);
	int ret = -EBUSY;

	mutex_lock(&dev->lock);
	if (dev->dead) {
		mutex_unlock(&dev->lock);
		return -ENODEV;
	}
	if (dev->connected)
		goto unlock;

	ret = 0;
	dev->connected = true;
	file->private_data = dev;
unlock:
	mutex_unlock(&dev->lock);
	cancel_delayed_work_sync(&dev->timeout_work);

	return ret;
}

static const struct file_operations vduse_dev_fops = {
	.owner		= THIS_MODULE,
	.open		= vduse_dev_open,
	.release	= vduse_dev_release,
	.read_iter	= vduse_dev_read_iter,
	.write_iter	= vduse_dev_write_iter,
	.poll		= vduse_dev_poll,
	.unlocked_ioctl	= vduse_dev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
	.mmap		= vduse_dev_mmap,
};

static ssize_t irq_use_wq_show(struct vduse_virtqueue *vq, char *buf)
{
	return sprintf(buf, "%d\n", vq->irq_use_wq);
}

static ssize_t irq_use_wq_store(struct vduse_virtqueue *vq,
				const char *buf, size_t count)
{
	bool enabled;
	int ret;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;

	vq->irq_use_wq = enabled;

	return count;
}

static ssize_t irq_affinity_show(struct vduse_virtqueue *vq, char *buf)
{
	return sprintf(buf, "%d\n", vq->irq_effective_cpu);
}

static ssize_t irq_affinity_store(struct vduse_virtqueue *vq,
				  const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 0, &val) < 0)
		return -EINVAL;

	if (!(val == -1 || (val <= nr_cpu_ids && val >= 0 && cpu_online(val))))
		return -EINVAL;

	vq->irq_effective_cpu = val;

	return count;
}

static ssize_t vq_affinity_show(struct vduse_virtqueue *vq, char *buf)
{
	return sprintf(buf, "%*pb\n", cpumask_pr_args(&vq->affinity));
}

static ssize_t vq_affinity_store(struct vduse_virtqueue *vq,
				     const char *buf, size_t count)
{
	cpumask_var_t new_value;
	int ret;

	if (!zalloc_cpumask_var(&new_value, GFP_KERNEL))
		return -ENOMEM;

	ret = cpumask_parse(buf, new_value);
	if (ret)
		goto free_mask;

	ret = -EINVAL;
	if (!cpumask_intersects(new_value, cpu_online_mask))
		goto free_mask;

	cpumask_copy(&vq->affinity, new_value);
	vq->automatic_affinity = false;
	ret = count;
free_mask:
	free_cpumask_var(new_value);
	return ret;
}

static ssize_t irq_inject_store(struct vduse_virtqueue *vq,
				const char *buf, size_t count)
{
	queue_work(vduse_irq_wq, &vq->inject);
	return count;
}

static ssize_t kick_store(struct vduse_virtqueue *vq,
			  const char *buff, size_t count)
{
	ssize_t ret = -EPERM;

	spin_lock(&vq->kick_lock);
	if (!vq->ready || !vq->kickfd)
		goto unlock;

	ret = count;
	eventfd_signal(vq->kickfd, 1);
unlock:
	spin_unlock(&vq->kick_lock);

	return ret;
}

struct vq_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct vduse_virtqueue *, char *);
	ssize_t (*store)(struct vduse_virtqueue *, const char *, size_t);
};

static struct vq_sysfs_entry irq_affinity_attr = __ATTR_RW(irq_affinity);

static struct vq_sysfs_entry vq_affinity_attr = __ATTR_RW(vq_affinity);

static struct vq_sysfs_entry irq_inject_attr = __ATTR_WO(irq_inject);

static struct vq_sysfs_entry kick_attr = __ATTR_WO(kick);

static struct vq_sysfs_entry irq_use_wq_attr = __ATTR_RW(irq_use_wq);

static struct attribute *vq_attrs[] = {
	&irq_affinity_attr.attr,
	&vq_affinity_attr.attr,
	&irq_inject_attr.attr,
	&kick_attr.attr,
	&irq_use_wq_attr.attr,
	NULL,
};

static ssize_t vq_attr_show(struct kobject *kobj, struct attribute *attr,
			    char *buf)
{
	struct vduse_virtqueue *vq = container_of(kobj,
					struct vduse_virtqueue, kobj);
	struct vq_sysfs_entry *entry = container_of(attr,
					struct vq_sysfs_entry, attr);

	if (!entry->show)
		return -EIO;

	return entry->show(vq, buf);
}

static ssize_t vq_attr_store(struct kobject *kobj, struct attribute *attr,
			     const char *buf, size_t count)
{
	struct vduse_virtqueue *vq = container_of(kobj,
					struct vduse_virtqueue, kobj);
	struct vq_sysfs_entry *entry = container_of(attr,
					struct vq_sysfs_entry, attr);

	if (!entry->store)
		return -EIO;

	return entry->store(vq, buf, count);
}

static const struct sysfs_ops vq_sysfs_ops = {
	.show = vq_attr_show,
	.store = vq_attr_store,
};

static void vq_release(struct kobject *kobj)
{
	struct vduse_virtqueue *vq = container_of(kobj,
					struct vduse_virtqueue, kobj);

	flush_work(&vq->inject);
	vringh_kiov_cleanup(&vq->out_iov);
	vringh_kiov_cleanup(&vq->in_iov);
	kfree(vq);
}

static struct kobj_type vq_attr_type = {
	.release	= vq_release,
	.sysfs_ops	= &vq_sysfs_ops,
	.default_attrs	= vq_attrs,
};

static void vduse_dev_deinit_vqs(struct vduse_dev *dev)
{
	int i;

	if (!dev->vqs)
		return;

	for (i = 0; i < dev->vq_num; i++) {
		dev->vqs[i]->dev = NULL;
		flush_work(&dev->vqs[i]->inject);
		kobject_put(&dev->vqs[i]->kobj);
	}
	kfree(dev->vqs);
}

static int vduse_dev_init_vqs(struct vduse_dev *dev, u32 vq_align,
			      u16 vq_size_max, u32 vq_num)
{
	int ret, i;

	dev->vq_align = vq_align;
	dev->vq_size_max = vq_size_max;
	dev->vq_num = vq_num;
	dev->vqs = kcalloc(dev->vq_num, sizeof(*dev->vqs), GFP_KERNEL);
	if (!dev->vqs)
		return -ENOMEM;

	for (i = 0; i < vq_num; i++) {
		dev->vqs[i] = kzalloc(sizeof(*dev->vqs[i]), GFP_KERNEL);
		if (!dev->vqs[i]) {
			i--;
			ret = -ENOMEM;
			goto err;
		}
		dev->vqs[i]->index = i;
		dev->vqs[i]->dev = dev;
		dev->vqs[i]->irq_effective_cpu = -1;
		dev->vqs[i]->automatic_affinity = true;
		cpumask_setall(&dev->vqs[i]->affinity);
		/* virtio-fs driver already uses workqueue in irq handler */
		dev->vqs[i]->irq_use_wq = (dev->device_id == VIRTIO_ID_FS) ?
					  false : true;

		vringh_set_iotlb(&dev->vqs[i]->vring, dev->domain->iotlb,
				 &dev->domain->iotlb_lock);
		vringh_kiov_init(&dev->vqs[i]->out_iov, NULL, 0);
		vringh_kiov_init(&dev->vqs[i]->in_iov, NULL, 0);
		INIT_WORK(&dev->vqs[i]->inject, vduse_vq_irq_inject_work);
		spin_lock_init(&dev->vqs[i]->kick_lock);
		init_rwsem(&dev->vqs[i]->irq_lock);
		kobject_init(&dev->vqs[i]->kobj, &vq_attr_type);
		ret = kobject_add(&dev->vqs[i]->kobj,
				  &dev->dev.kobj, "vq%d", i);
		if (ret)
			goto err;
	}

	return 0;
err:
	for (; i >= 0; i--)
		kobject_put(&dev->vqs[i]->kobj);
	kfree(dev->vqs);
	dev->vqs = NULL;
	return ret;
}

static int vduse_blk_timeout_handler(struct vduse_dev *dev,
				     struct vduse_virtqueue *vq)
{
	size_t len = 0;
	ssize_t bytes;
	unsigned short head = vq->vring.vring.num;
	u8 status;
	int ret;

	ret = vringh_getdesc_iotlb(&vq->vring, &vq->out_iov, &vq->in_iov,
				   &head, GFP_ATOMIC);
	if (ret != 1) {
		if (ret < 0 && head != vq->vring.vring.num)
			goto err;
		return ret;
	}

	if (vq->out_iov.used < 1 || vq->in_iov.used < 1) {
		pr_err("VDUSE: missing headers - out_iov: %u in_iov %u\n",
		       vq->out_iov.used, vq->in_iov.used);
		goto out;
	}

	if (vq->in_iov.iov[vq->in_iov.used - 1].iov_len < 1) {
		pr_err("VDUSE: request in header too short\n");
		goto out;
	}

	len = vringh_kiov_length(&vq->in_iov);
	status = VIRTIO_BLK_S_IOERR;

	vringh_kiov_advance(&vq->in_iov, len - 1);

	/* Last byte is the status */
	bytes = vringh_iov_push_iotlb(&vq->vring, &vq->in_iov, &status, 1);
	if (bytes != 1) {
		ret = (bytes >= 0) ? -EINVAL : bytes;
		pr_err("VDUSE: update status failed: %ld\n", bytes);
		goto err;
	}

	/* Make sure data is wrote before advancing index */
	smp_wmb();
out:
	ret = vringh_complete_iotlb(&vq->vring, head, len);
	if (ret) {
		pr_err("VDUSE: update used vring failed\n");
		goto err;
	}

	return 1;
err:
	vringh_abandon_iotlb(&vq->vring, 1);
	return ret;
}

static int vduse_fs_timeout_handler(struct vduse_dev *dev,
				    struct vduse_virtqueue *vq)
{
	size_t len = 0;
	ssize_t bytes;
	unsigned short head = vq->vring.vring.num;
	int ret;
	struct fuse_out_header out;

	ret = vringh_getdesc_iotlb(&vq->vring, &vq->out_iov, &vq->in_iov,
				   &head, GFP_ATOMIC);
	if (ret != 1) {
		if (ret < 0 && head != vq->vring.vring.num)
			goto err;
		return ret;
	}

	len = vringh_kiov_length(&vq->in_iov);
	if (!len)
		goto out;

	if (vq->index == 0) {
		pr_err("VDUSE: invalid req in virtiofs high priority queue\n");
		goto out;
	}

	out.error = -EIO;
	bytes = vringh_iov_push_iotlb(&vq->vring, &vq->in_iov,
				      &out, sizeof(struct fuse_out_header));
	if (bytes != sizeof(struct fuse_out_header)) {
		ret = (bytes >= 0) ? -EINVAL : bytes;
		pr_err("VDUSE: write fuse header failed in (%u, %u): %ld\n",
		       head, vq->vring.last_avail_idx, bytes);
		goto err;
	}

	/* Make sure data is wrote before advancing index */
	smp_wmb();
out:
	ret = vringh_complete_iotlb(&vq->vring, head, len);
	if (ret) {
		pr_err("VDUSE: update used vring failed in (%u, %u): %ld\n",
		       head, vq->vring.last_avail_idx, bytes);
		goto err;
	}

	return 1;
err:
	vringh_abandon_iotlb(&vq->vring, 1);
	return ret;
}

static int vduse_default_timeout_handler(struct vduse_dev *dev,
					 struct vduse_virtqueue *vq)
{
	size_t len = 0;
	unsigned short head;
	int ret;

	ret = vringh_getdesc_iotlb(&vq->vring, &vq->out_iov, &vq->in_iov,
				   &head, GFP_ATOMIC);
	if (ret != 1) {
		if (ret < 0 && head != vq->vring.vring.num)
			goto err;
		return ret;
	}

	ret = vringh_complete_iotlb(&vq->vring, head, len);
	if (ret) {
		pr_err("VDUSE: update used vring failed\n");
		goto err;
	}

	return 1;
err:
	vringh_abandon_iotlb(&vq->vring, 1);
	return ret;
}

/* Returns 0 if there was no request, 1 if there was, or -errno. */
static int vduse_req_timeout_handler(struct vduse_dev *dev,
				     struct vduse_virtqueue *vq)
{
	int ret;
	bool do_retry = true;

retry:
	ret = vduse_domain_translate_map(dev->domain);
	if (ret) {
		pr_err("VDUSE: translate domain mapping failed\n");
		return ret;
	}

	ret = dev->dead_handler(dev, vq);
	if (ret < 0 && do_retry) {
		do_retry = false;
		goto retry;
	}

	return ret;
}

static void vduse_vq_check_inflights(struct vduse_dev *dev, int index)
{
	struct vduse_vq_inflight *inflight = vduse_get_vq_inflight(dev, index);
	struct vduse_virtqueue *vq = dev->vqs[index];
	uint16_t idx = vq->vring.last_avail_idx;
	int i, desc_num = dev->vq_size_max;

	if (inflight->desc_num)
		desc_num = inflight->desc_num;

	if (inflight->used_idx != idx)
		inflight->desc[inflight->last_batch_head].inflight = 0;

	for (i = 0; i < desc_num; i++) {
		if (inflight->desc[i].inflight)
			vringh_recover_desc_iotlb(&vq->vring, idx++, i);
	}

	pr_info("VDUSE: get vq%d I/O for %s, desc %d num %d idx %u inuse %d\n",
		index, dev_name(&dev->dev), desc_num, vq->vring.vring.num,
		vq->vring.last_avail_idx, idx - vq->vring.last_avail_idx);
}

static void vduse_dev_timeout_work(struct work_struct *work)
{
	int i, ret;
	struct vduse_dev_msg *msg;
	struct vduse_dev *dev = container_of(to_delayed_work(work),
					struct vduse_dev, timeout_work);
	bool check_inflight = false;

	mutex_lock(&dev->lock);
	if (dev->connected && !dev->aborted)
		goto unlock;

	if (!dev->dead) {
		pr_warn("VDUSE: dead connection found in %s\n",
			dev_name(&dev->dev));
		check_inflight = true;
		flush_workqueue(vduse_irq_wq);
		flush_workqueue(vduse_irq_bound_wq);
	}
	spin_lock(&dev->msg_lock);
	dev->dead = true;
	while ((msg = vduse_dequeue_msg(&dev->send_list))) {
		msg->resp.result = VDUSE_REQUEST_FAILED;
		msg->completed = 1;
		wake_up(&msg->waitq);
	}
	spin_unlock(&dev->msg_lock);

	if (!dev->dead_handler)
		goto unlock;

	if (!dev->shm_addr && check_inflight) {
		check_inflight = false;
		pr_warn("VDUSE: can't check inflight I/Os in %s\n",
			dev_name(&dev->dev));
	}

	for (i = 0; i < dev->vq_num; i++) {
		if (!dev->vqs[i]->ready)
			continue;

		if (vringh_recover_iotlb(&dev->vqs[i]->vring))
			continue;

		if (check_inflight)
			vduse_vq_check_inflights(dev, i);

		do {
			vringh_notify_disable_iotlb(&dev->vqs[i]->vring);
			while ((ret = vduse_req_timeout_handler(dev,
				dev->vqs[i])) == 1)
				;
		} while (!vringh_notify_enable_iotlb(&dev->vqs[i]->vring) &&
			 ret >= 0);
		vduse_vq_irq_inject(dev->vqs[i]);
	}
unlock:
	mutex_unlock(&dev->lock);
}

static void vduse_set_dead_handler(struct vduse_dev *dev)
{
	if (dev->device_id == VIRTIO_ID_FS)
		dev->dead_handler = vduse_fs_timeout_handler;
	else if (dev->device_id == VIRTIO_ID_BLOCK)
		dev->dead_handler = vduse_blk_timeout_handler;
	else
		dev->dead_handler = vduse_default_timeout_handler;
}

static ssize_t abort_conn_store(struct device *device,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct vduse_dev *dev = container_of(device, struct vduse_dev, dev);

	dev->aborted = true;
	mod_delayed_work(system_wq, &dev->timeout_work, 0);

	return count;
}

static DEVICE_ATTR_WO(abort_conn);

static ssize_t enable_dead_handler_show(struct device *device,
					struct device_attribute *attr,
					char *buf)
{
	struct vduse_dev *dev = container_of(device, struct vduse_dev, dev);

	return sprintf(buf, "%d\n", dev->dead_handler ? 1 : 0);
}

static ssize_t enable_dead_handler_store(struct device *device,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct vduse_dev *dev = container_of(device, struct vduse_dev, dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	mutex_lock(&dev->lock);
	if (enable)
		vduse_set_dead_handler(dev);
	else
		dev->dead_handler = NULL;
	mutex_unlock(&dev->lock);

	return count;
}

static DEVICE_ATTR_RW(enable_dead_handler);

static ssize_t dead_timeout_show(struct device *device,
				 struct device_attribute *attr,
				 char *buf)
{
	struct vduse_dev *dev = container_of(device, struct vduse_dev, dev);

	return sprintf(buf, "%u\n", dev->dead_timeout);
}

static ssize_t dead_timeout_store(struct device *device,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct vduse_dev *dev = container_of(device, struct vduse_dev, dev);
	int ret;

	ret = kstrtou16(buf, 0, &dev->dead_timeout);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(dead_timeout);

static struct attribute *vduse_dev_attrs[] = {
	&dev_attr_abort_conn.attr,
	&dev_attr_enable_dead_handler.attr,
	&dev_attr_dead_timeout.attr,
	NULL
};

ATTRIBUTE_GROUPS(vduse_dev);

static struct vduse_dev *vduse_dev_create(void)
{
	struct vduse_dev *dev = kzalloc(sizeof(*dev), GFP_KERNEL);

	if (!dev)
		return NULL;

	mutex_init(&dev->lock);
	mutex_init(&dev->mem_lock);
	spin_lock_init(&dev->config_lock);
	spin_lock_init(&dev->msg_lock);
	INIT_LIST_HEAD(&dev->send_list);
	INIT_LIST_HEAD(&dev->recv_list);
	atomic64_set(&dev->msg_unique, 0);
	spin_lock_init(&dev->irq_lock);

	INIT_WORK(&dev->inject, vduse_dev_irq_inject);
	init_waitqueue_head(&dev->waitq);
	INIT_DELAYED_WORK(&dev->timeout_work, vduse_dev_timeout_work);

	return dev;
}

static void vduse_dev_destroy(struct vduse_dev *dev)
{
	kfree(dev->config);
	vfree(dev->shm_addr);
	kfree(dev);
}

static struct vduse_dev *vduse_find_dev(const char *name)
{
	struct vduse_dev *tmp, *dev = NULL;

	list_for_each_entry(tmp, &vduse_devs, list) {
		if (!strcmp(tmp->name, name)) {
			dev = tmp;
			break;
		}
	}
	return dev;
}

static int vduse_destroy_dev(char *name)
{
	struct vduse_dev *dev = vduse_find_dev(name);

	if (!dev)
		return -EINVAL;

	mutex_lock(&dev->lock);
	if (dev->vdev || dev->connected) {
		mutex_unlock(&dev->lock);
		return -EBUSY;
	}
	dev->connected = true;
	mutex_unlock(&dev->lock);
	cancel_delayed_work_sync(&dev->timeout_work);

	list_del(&dev->list);
	cdev_device_del(&dev->cdev, &dev->dev);
	vduse_dev_deinit_vqs(dev);
	put_device(&dev->dev);
	module_put(THIS_MODULE);

	return 0;
}

static void vduse_release_dev(struct device *device)
{
	struct vduse_dev *dev =
		container_of(device, struct vduse_dev, dev);

	flush_work(&dev->inject);
	ida_simple_remove(&vduse_ida, dev->minor);
	vduse_domain_destroy(dev->domain);
	kfree(dev->name);
	vduse_dev_destroy(dev);
}

static bool device_is_allowed(u32 device_id)
{
	int i;

	if (allow_unsafe_device || device_id >= MIN_CUSTOM_VIRTIO_ID)
		return true;

	for (i = 0; i < ARRAY_SIZE(allowed_device_id); i++)
		if (allowed_device_id[i] == device_id)
			return true;

	return false;
}

static bool vduse_validate_config(struct vduse_dev_config *config)
{
	if (config->vq_align > PAGE_SIZE)
		return false;

	if (config->config_size > PAGE_SIZE)
		return false;

	if (config->vq_num > 0xffff)
		return false;

	if (!device_is_allowed(config->device_id))
		return false;

	return true;
}

static int vduse_create_dev(struct vduse_dev_config *config,
			    unsigned long api_version)
{
	int ret = -ENOMEM;
	struct vduse_dev *dev;

	if (config->bounce_size > max_bounce_size)
		return -EINVAL;

	if (vduse_find_dev(config->name))
		return -EEXIST;

	dev = vduse_dev_create();
	if (!dev)
		return -ENOMEM;

	dev->api_version = api_version;
	dev->dead_timeout = config->dead_timeout;
	dev->device_id = config->device_id;
	dev->vendor_id = config->vendor_id;
	dev->config_size = config->config_size;
	dev->dev_shm_size = config->dev_shm_size;
	dev->vq_shm_off = config->vq_shm_off;
	dev->req_cached = config->req_cached;
	dev->name = kstrdup(config->name, GFP_KERNEL);
	if (!dev->name)
		goto err_str;

	dev->domain = vduse_domain_create(max_iova_size - 1,
					config->bounce_size);
	if (!dev->domain)
		goto err_domain;

	ret = ida_simple_get(&vduse_ida, 0, VDUSE_DEV_MAX, GFP_KERNEL);
	if (ret < 0)
		goto err_ida;

	dev->minor = ret;
	device_initialize(&dev->dev);
	dev->dev.release = vduse_release_dev;
	dev->dev.groups = vduse_dev_groups;
	dev->dev.class = vduse_class;
	dev->dev.devt = MKDEV(MAJOR(vduse_major), dev->minor);
	ret = dev_set_name(&dev->dev, "%s", config->name);
	if (ret)
		goto err;

	cdev_init(&dev->cdev, &vduse_dev_fops);
	dev->cdev.owner = THIS_MODULE;

	ret = cdev_device_add(&dev->cdev, &dev->dev);
	if (ret)
		goto err;

	ret = vduse_dev_init_vqs(dev, config->vq_align,
				 config->vq_size_max, config->vq_num);
	if (ret)
		goto err_vqs;

	vduse_set_dead_handler(dev);
	list_add(&dev->list, &vduse_devs);
	__module_get(THIS_MODULE);

	return 0;
err_ida:
	vduse_domain_destroy(dev->domain);
err_domain:
	kfree(dev->name);
err_str:
	vduse_dev_destroy(dev);
	return ret;
err_vqs:
	cdev_device_del(&dev->cdev, &dev->dev);
err:
	put_device(&dev->dev);
	return ret;
}

static long vduse_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	int ret;
	void __user *argp = (void __user *)arg;
	struct vduse_control *control = file->private_data;

	mutex_lock(&vduse_lock);
	switch (cmd) {
	case VDUSE_GET_API_VERSION:
		ret = control->api_version;
		break;
	case VDUSE_SET_API_VERSION:
		ret = -EINVAL;
		if (arg > VDUSE_API_VERSION)
			break;

		ret = 0;
		control->api_version = arg;
		break;
	case VDUSE_CREATE_DEV: {
		struct vduse_dev_config config;

		ret = -EFAULT;
		if (copy_from_user(&config, argp, sizeof(config)))
			break;

		ret = -EINVAL;
		if (vduse_validate_config(&config) == false)
			break;

		ret = vduse_create_dev(&config, control->api_version);
		break;
	}
	case VDUSE_DESTROY_DEV: {
		char name[VDUSE_NAME_MAX];

		ret = -EFAULT;
		if (copy_from_user(name, argp, VDUSE_NAME_MAX))
			break;

		ret = vduse_destroy_dev(name);
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&vduse_lock);

	return ret;
}

static int vduse_release(struct inode *inode, struct file *file)
{
	struct vduse_control *control = file->private_data;

	kfree(control);
	return 0;
}

static int vduse_open(struct inode *inode, struct file *file)
{
	struct vduse_control *control;

	control = kmalloc(sizeof(struct vduse_control), GFP_KERNEL);
	if (!control)
		return -ENOMEM;

	control->api_version = VDUSE_API_VERSION;
	file->private_data = control;

	return 0;
}

static const struct file_operations vduse_fops = {
	.owner		= THIS_MODULE,
	.open		= vduse_open,
	.release	= vduse_release,
	.unlocked_ioctl	= vduse_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static char *vduse_devnode(struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;

	return kasprintf(GFP_KERNEL, "vduse/%s", dev_name(dev));
}

static struct miscdevice vduse_misc = {
	.fops = &vduse_fops,
	.minor = MISC_DYNAMIC_MINOR,
	.name = "vduse",
	.nodename = "vduse/control",
	.mode = 0666,
};

static void vduse_mgmtdev_release(struct device *dev)
{
}

static struct device vduse_mgmtdev = {
	.init_name = "vduse",
	.release = vduse_mgmtdev_release,
};

static struct vdpa_mgmt_dev mgmt_dev;

static int vduse_dev_init_vdpa(struct vduse_dev *dev, const char *name)
{
	struct vduse_vdpa *vdev;
	int ret;

	if (dev->vdev)
		return -EEXIST;

	vdev = vdpa_alloc_device(struct vduse_vdpa, vdpa, &dev->dev,
				 &vduse_vdpa_config_ops, name, true);
	if (!vdev)
		return -ENOMEM;

	dev->vdev = vdev;
	vdev->dev = dev;
	mutex_init(&vdev->lock);
	vdev->vdpa.dev.dma_mask = &vdev->vdpa.dev.coherent_dma_mask;
	ret = dma_set_mask_and_coherent(&vdev->vdpa.dev, DMA_BIT_MASK(64));
	if (ret) {
		put_device(&vdev->vdpa.dev);
		return ret;
	}
	set_dma_ops(&vdev->vdpa.dev, &vduse_dev_dma_ops);
	vdev->vdpa.dma_dev = &vdev->vdpa.dev;
	vdev->vdpa.mdev = &mgmt_dev;
	list_add(&vdev->vdpa_list, &vdpa_devs);

	return 0;
}

static int vdpa_dev_add(struct vdpa_mgmt_dev *mdev, const char *name)
{
	struct vduse_dev *dev;
	int ret;

	mutex_lock(&vduse_lock);
	dev = vduse_find_dev(name);
	if (!dev) {
		mutex_unlock(&vduse_lock);
		return -EINVAL;
	}
	ret = vduse_dev_init_vdpa(dev, name);
	mutex_unlock(&vduse_lock);
	if (ret)
		return ret;

	mutex_lock(&dev->vdev->lock);
	ret = _vdpa_register_device(&dev->vdev->vdpa, dev->vq_num);
	mutex_unlock(&dev->vdev->lock);
	if (ret) {
		mutex_lock(&vduse_lock);
		list_del(&dev->vdev->vdpa_list);
		mutex_unlock(&vduse_lock);
		put_device(&dev->vdev->vdpa.dev);
		return ret;
	}

	return 0;
}

static void vdpa_dev_del_common(struct vdpa_mgmt_dev *mdev,
				struct vdpa_device *dev, int timeout)
{
	struct vduse_dev *vdev = vdpa_to_vduse(dev);

	if (timeout >= 0) {
		vdev->aborted = true;
		mod_delayed_work(system_wq, &vdev->timeout_work,
				 msecs_to_jiffies(timeout * 1000));
	}
	mutex_lock(&vdev->vdev->lock);
	_vdpa_unregister_device(dev);
	mutex_unlock(&vdev->vdev->lock);
}

static void vdpa_dev_del(struct vdpa_mgmt_dev *mdev,
			 struct vdpa_device *dev, int timeout)
{
	struct vduse_dev *vdev = vdpa_to_vduse(dev);

	mutex_lock(&vduse_lock);
	list_del(&vdev->vdev->vdpa_list);
	mutex_unlock(&vduse_lock);
	vdpa_dev_del_common(mdev, dev, timeout);
}

static int vdpa_dev_del_parallel(struct vdpa_mgmt_dev *mdev,
				 struct vdpa_device *dev, int timeout)
{
	struct vduse_vdpa *tmp = NULL;
	struct vduse_dev *vdev = vdpa_to_vduse(dev);

	mutex_lock(&vduse_lock);
	list_for_each_entry(tmp, &vdpa_devs, vdpa_list) {
		if (tmp == vdev->vdev)
			break;
	}
	if (!tmp || tmp != vdev->vdev) {
		mutex_unlock(&vduse_lock);
		return -ENODEV;
	}
	list_del(&vdev->vdev->vdpa_list);
	mutex_unlock(&vduse_lock);
	vdpa_dev_del_common(mdev, dev, timeout);

	return 0;
}

static const struct vdpa_mgmtdev_ops vdpa_dev_mgmtdev_ops = {
	.dev_add = vdpa_dev_add,
	.dev_del = vdpa_dev_del,
	.dev_add_parallel = vdpa_dev_add,
	.dev_del_parallel = vdpa_dev_del_parallel,
};

static struct virtio_device_id id_table[] = {
	{ VIRTIO_DEV_ANY_ID, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct vdpa_mgmt_dev mgmt_dev = {
	.device = &vduse_mgmtdev,
	.id_table = id_table,
	.ops = &vdpa_dev_mgmtdev_ops,
};

static int vduse_mgmtdev_init(void)
{
	int ret;

	ret = device_register(&vduse_mgmtdev);
	if (ret)
		return ret;

	ret = vdpa_mgmtdev_register(&mgmt_dev);
	if (ret)
		goto err;

	return 0;
err:
	device_unregister(&vduse_mgmtdev);
	return ret;
}

static void vduse_mgmtdev_exit(void)
{
	vdpa_mgmtdev_unregister(&mgmt_dev);
	device_unregister(&vduse_mgmtdev);
}

static int vduse_init(void)
{
	int ret;

	if (max_bounce_size >= max_iova_size)
		return -EINVAL;

	ret = misc_register(&vduse_misc);
	if (ret)
		return ret;

	vduse_class = class_create(THIS_MODULE, "vduse");
	if (IS_ERR(vduse_class)) {
		ret = PTR_ERR(vduse_class);
		goto err_class;
	}
	vduse_class->devnode = vduse_devnode;

	ret = alloc_chrdev_region(&vduse_major, 0, VDUSE_DEV_MAX, "vduse");
	if (ret)
		goto err_chardev;

	vduse_irq_wq = alloc_workqueue("vduse-irq",
				WQ_HIGHPRI | WQ_SYSFS | WQ_UNBOUND, 0);
	if (!vduse_irq_wq)
		goto err_wq;

	vduse_irq_bound_wq = alloc_workqueue("vduse-irq-bound", WQ_HIGHPRI, 0);
	if (!vduse_irq_bound_wq)
		goto err_bound_wq;

	ret = vduse_domain_init();
	if (ret)
		goto err_domain;

	ret = vduse_mgmtdev_init();
	if (ret)
		goto err_mgmtdev;

	return 0;
err_mgmtdev:
	vduse_domain_exit();
err_domain:
	destroy_workqueue(vduse_irq_bound_wq);
err_bound_wq:
	destroy_workqueue(vduse_irq_wq);
err_wq:
	unregister_chrdev_region(vduse_major, VDUSE_DEV_MAX);
err_chardev:
	class_destroy(vduse_class);
err_class:
	misc_deregister(&vduse_misc);
	return ret;
}
module_init(vduse_init);

static void vduse_exit(void)
{
	misc_deregister(&vduse_misc);
	class_destroy(vduse_class);
	unregister_chrdev_region(vduse_major, VDUSE_DEV_MAX);
	destroy_workqueue(vduse_irq_wq);
	destroy_workqueue(vduse_irq_bound_wq);
	vduse_domain_exit();
	vduse_mgmtdev_exit();
}
module_exit(vduse_exit);

MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE(DRV_LICENSE);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESC);
