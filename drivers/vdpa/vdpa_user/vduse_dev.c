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
#include <uapi/linux/vduse.h>
#include <uapi/linux/vdpa.h>
#include <uapi/linux/virtio_config.h>
#include <linux/mod_devicetable.h>

#include "iova_domain.h"

#define DRV_VERSION  "1.0"
#define DRV_AUTHOR   "Yongji Xie <xieyongji@bytedance.com>"
#define DRV_DESC     "vDPA Device in Userspace"
#define DRV_LICENSE  "GPL v2"

#define VDUSE_DEV_MAX (1U << MINORBITS)

struct vduse_virtqueue {
	u16 index;
	bool ready;
	spinlock_t kick_lock;
	spinlock_t irq_lock;
	struct eventfd_ctx *kickfd;
	struct vdpa_callback cb;
	struct work_struct inject;
};

struct vduse_dev;

struct vduse_vdpa {
	struct vdpa_device vdpa;
	struct vduse_dev *dev;
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
	int minor;
	u16 vq_size_max;
	u32 vq_num;
	u32 vq_align;
	u32 device_id;
	u32 vendor_id;
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

static unsigned long max_bounce_size = (64 * 1024 * 1024);
module_param(max_bounce_size, ulong, 0444);
MODULE_PARM_DESC(max_bounce_size, "Maximum bounce buffer size. (default: 64M)");

static unsigned long max_iova_size = (128 * 1024 * 1024);
module_param(max_iova_size, ulong, 0444);
MODULE_PARM_DESC(max_iova_size, "Maximum iova space size (default: 128M)");

static DEFINE_MUTEX(vduse_lock);
static LIST_HEAD(vduse_devs);
static DEFINE_IDA(vduse_ida);

static dev_t vduse_major;
static struct class *vduse_class;
static struct workqueue_struct *vduse_irq_wq;

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
	init_waitqueue_head(&msg->waitq);
	spin_lock(&dev->msg_lock);
	vduse_enqueue_msg(&dev->send_list, msg);
	wake_up(&dev->waitq);
	spin_unlock(&dev->msg_lock);
	wait_event_killable(msg->waitq, msg->completed);
	spin_lock(&dev->msg_lock);
	if (!msg->completed) {
		list_del(&msg->list);
		msg->resp.result = VDUSE_REQUEST_FAILED;
	}
	spin_unlock(&dev->msg_lock);

	return (msg->resp.result == VDUSE_REQUEST_OK) ? 0 : -1;
}

static u32 vduse_dev_get_request_id(struct vduse_dev *dev)
{
	return atomic64_fetch_inc(&dev->msg_unique);
}

static u64 vduse_dev_get_features(struct vduse_dev *dev)
{
	struct vduse_dev_msg msg = { 0 };

	msg.req.type = VDUSE_GET_FEATURES;
	msg.req.request_id = vduse_dev_get_request_id(dev);

	if (WARN_ON(vduse_dev_msg_sync(dev, &msg) != 0))
		return 0;

	return msg.resp.f.features;
}

static int vduse_dev_set_features(struct vduse_dev *dev, u64 features)
{
	struct vduse_dev_msg msg = { 0 };

	msg.req.type = VDUSE_SET_FEATURES;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.f.features = features;

	return vduse_dev_msg_sync(dev, &msg);
}

static u8 vduse_dev_get_status(struct vduse_dev *dev)
{
	struct vduse_dev_msg msg = { 0 };

	msg.req.type = VDUSE_GET_STATUS;
	msg.req.request_id = vduse_dev_get_request_id(dev);

	if (WARN_ON(vduse_dev_msg_sync(dev, &msg) != 0))
		return 0;

	return msg.resp.s.status;
}

static void vduse_dev_set_status(struct vduse_dev *dev, u8 status)
{
	struct vduse_dev_msg msg = { 0 };

	msg.req.type = VDUSE_SET_STATUS;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.s.status = status;

	WARN_ON(vduse_dev_msg_sync(dev, &msg) != 0);
}

static void vduse_dev_get_config(struct vduse_dev *dev, unsigned int offset,
				 void *buf, unsigned int len)
{
	struct vduse_dev_msg msg = { 0 };
	unsigned int sz;

	while (len) {
		sz = min_t(unsigned int, len, sizeof(msg.req.config.data));
		msg.req.type = VDUSE_GET_CONFIG;
		msg.req.request_id = vduse_dev_get_request_id(dev);
		msg.req.config.offset = offset;
		msg.req.config.len = sz;
		if (WARN_ON(vduse_dev_msg_sync(dev, &msg) != 0))
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
	struct vduse_dev_msg msg = { 0 };
	unsigned int sz;

	while (len) {
		sz = min_t(unsigned int, len, sizeof(msg.req.config.data));
		msg.req.type = VDUSE_SET_CONFIG;
		msg.req.request_id = vduse_dev_get_request_id(dev);
		msg.req.config.offset = offset;
		msg.req.config.len = sz;
		memcpy(msg.req.config.data, buf, sz);
		if (WARN_ON(vduse_dev_msg_sync(dev, &msg) != 0))
			break;

		buf += sz;
		offset += sz;
		len -= sz;
	}
}

static void vduse_dev_set_vq_num(struct vduse_dev *dev,
				 struct vduse_virtqueue *vq, u32 num)
{
	struct vduse_dev_msg msg = { 0 };

	msg.req.type = VDUSE_SET_VQ_NUM;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.vq_num.index = vq->index;
	msg.req.vq_num.num = num;

	WARN_ON(vduse_dev_msg_sync(dev, &msg) != 0);
}

static int vduse_dev_set_vq_addr(struct vduse_dev *dev,
				 struct vduse_virtqueue *vq, u64 desc_addr,
				 u64 driver_addr, u64 device_addr)
{
	struct vduse_dev_msg msg = { 0 };

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
	struct vduse_dev_msg msg = { 0 };

	msg.req.type = VDUSE_SET_VQ_READY;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.vq_ready.index = vq->index;
	msg.req.vq_ready.ready = ready;

	WARN_ON(vduse_dev_msg_sync(dev, &msg) != 0);
}

static bool vduse_dev_get_vq_ready(struct vduse_dev *dev,
				   struct vduse_virtqueue *vq)
{
	struct vduse_dev_msg msg = { 0 };

	msg.req.type = VDUSE_GET_VQ_READY;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.vq_ready.index = vq->index;
	if (WARN_ON(vduse_dev_msg_sync(dev, &msg)))
		return false;

	return msg.resp.vq_ready.ready;
}

static int vduse_dev_get_vq_state(struct vduse_dev *dev,
				struct vduse_virtqueue *vq,
				struct vdpa_vq_state *state)
{
	struct vduse_dev_msg msg = { 0 };
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
	struct vduse_dev_msg msg = { 0 };

	msg.req.type = VDUSE_SET_VQ_STATE;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.vq_state.index = vq->index;
	msg.req.vq_state.avail_idx = state->avail_index;

	return vduse_dev_msg_sync(dev, &msg);
}

static int vduse_dev_update_iotlb(struct vduse_dev *dev,
				u64 start, u64 last)
{
	struct vduse_dev_msg msg = { 0 };

	if (last < start)
		return -EINVAL;

	msg.req.type = VDUSE_UPDATE_IOTLB;
	msg.req.request_id = vduse_dev_get_request_id(dev);
	msg.req.iova.start = start;
	msg.req.iova.last = last;

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

	spin_lock(&dev->irq_lock);
	dev->config_cb.callback = NULL;
	dev->config_cb.private = NULL;
	spin_unlock(&dev->irq_lock);

	for (i = 0; i < dev->vq_num; i++) {
		struct vduse_virtqueue *vq = dev->vqs[i];

		spin_lock(&vq->irq_lock);
		vq->ready = false;
		vq->cb.callback = NULL;
		vq->cb.private = NULL;
		spin_unlock(&vq->irq_lock);
	}
}

static int vduse_vdpa_set_vq_address(struct vdpa_device *vdpa, u16 idx,
				u64 desc_area, u64 driver_area,
				u64 device_area)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	return vduse_dev_set_vq_addr(dev, vq, desc_area,
					driver_area, device_area);
}

static void vduse_vdpa_kick_vq(struct vdpa_device *vdpa, u16 idx)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	spin_lock(&vq->kick_lock);
	if (vq->ready && vq->kickfd)
		eventfd_signal(vq->kickfd, 1);
	spin_unlock(&vq->kick_lock);
}

static void vduse_vdpa_set_vq_cb(struct vdpa_device *vdpa, u16 idx,
			      struct vdpa_callback *cb)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	spin_lock(&vq->irq_lock);
	vq->cb.callback = cb->callback;
	vq->cb.private = cb->private;
	spin_unlock(&vq->irq_lock);
}

static void vduse_vdpa_set_vq_num(struct vdpa_device *vdpa, u16 idx, u32 num)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	vduse_dev_set_vq_num(dev, vq, num);
}

static void vduse_vdpa_set_vq_ready(struct vdpa_device *vdpa,
					u16 idx, bool ready)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	vduse_dev_set_vq_ready(dev, vq, ready);
	vq->ready = ready;
}

static bool vduse_vdpa_get_vq_ready(struct vdpa_device *vdpa, u16 idx)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	vq->ready = vduse_dev_get_vq_ready(dev, vq);

	return vq->ready;
}

static int vduse_vdpa_set_vq_state(struct vdpa_device *vdpa, u16 idx,
				const struct vdpa_vq_state *state)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);
	struct vduse_virtqueue *vq = dev->vqs[idx];

	return vduse_dev_set_vq_state(dev, vq, state);
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

	return vduse_dev_get_features(dev);
}

static int vduse_vdpa_set_features(struct vdpa_device *vdpa, u64 features)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	if (!(features & (1ULL << VIRTIO_F_ACCESS_PLATFORM)))
		return -EINVAL;

	return vduse_dev_set_features(dev, features);
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

	return vduse_dev_get_status(dev);
}

static void vduse_vdpa_set_status(struct vdpa_device *vdpa, u8 status)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	vduse_dev_set_status(dev, status);

	if (status == 0)
		vduse_dev_reset(dev);
}

static void vduse_vdpa_get_config(struct vdpa_device *vdpa, unsigned int offset,
			     void *buf, unsigned int len)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	vduse_dev_get_config(dev, offset, buf, len);
}

static void vduse_vdpa_set_config(struct vdpa_device *vdpa, unsigned int offset,
			const void *buf, unsigned int len)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	vduse_dev_set_config(dev, offset, buf, len);
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

static void vduse_vdpa_free(struct vdpa_device *vdpa)
{
	struct vduse_dev *dev = vdpa_to_vduse(vdpa);

	WARN_ON(!list_empty(&dev->send_list));
	WARN_ON(!list_empty(&dev->recv_list));
	dev->vdev = NULL;
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
	.get_config		= vduse_vdpa_get_config,
	.set_config		= vduse_vdpa_set_config,
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

	spin_lock_irq(&dev->irq_lock);
	if (dev->config_cb.callback)
		dev->config_cb.callback(dev->config_cb.private);
	spin_unlock_irq(&dev->irq_lock);
}

static void vduse_vq_irq_inject(struct work_struct *work)
{
	struct vduse_virtqueue *vq = container_of(work,
					struct vduse_virtqueue, inject);

	spin_lock_irq(&vq->irq_lock);
	if (vq->ready && vq->cb.callback)
		vq->cb.callback(vq->cb.private);
	spin_unlock_irq(&vq->irq_lock);
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

		ret = -EFAULT;
		if (copy_from_user(&vq_index, argp, sizeof(u32)))
			break;

		ret = -EINVAL;
		if (vq_index >= dev->vq_num)
			break;

		ret = 0;
		vq_index = array_index_nospec(vq_index, dev->vq_num);
		queue_work(vduse_irq_wq, &dev->vqs[vq_index]->inject);
		break;
	}
	case VDUSE_INJECT_CONFIG_IRQ:
		ret = 0;
		queue_work(vduse_irq_wq, &dev->inject);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

static int vduse_dev_release(struct inode *inode, struct file *file)
{
	struct vduse_dev *dev = file->private_data;
	struct vduse_dev_msg *msg;
	int i;

	for (i = 0; i < dev->vq_num; i++) {
		struct vduse_virtqueue *vq = dev->vqs[i];

		spin_lock(&vq->kick_lock);
		if (vq->kickfd)
			eventfd_ctx_put(vq->kickfd);
		vq->kickfd = NULL;
		spin_unlock(&vq->kick_lock);
	}

	spin_lock(&dev->msg_lock);
	/* Make sure the inflight messages can processed after reconncection */
	while ((msg = vduse_dequeue_msg(&dev->recv_list)))
		vduse_enqueue_msg(&dev->send_list, msg);
	spin_unlock(&dev->msg_lock);

	dev->connected = false;

	return 0;
}

static int vduse_dev_open(struct inode *inode, struct file *file)
{
	struct vduse_dev *dev = container_of(inode->i_cdev,
					struct vduse_dev, cdev);
	int ret = -EBUSY;

	mutex_lock(&dev->lock);
	if (dev->connected)
		goto unlock;

	ret = 0;
	dev->connected = true;
	file->private_data = dev;
unlock:
	mutex_unlock(&dev->lock);

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
};

static void vduse_dev_deinit_vqs(struct vduse_dev *dev)
{
	int i;

	if (!dev->vqs)
		return;

	for (i = 0; i < dev->vq_num; i++)
		kfree(dev->vqs[i]);
	kfree(dev->vqs);
}

static int vduse_dev_init_vqs(struct vduse_dev *dev, u32 vq_align,
			      u16 vq_size_max, u32 vq_num)
{
	int i;

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
			goto err;
		}
		dev->vqs[i]->index = i;
		INIT_WORK(&dev->vqs[i]->inject, vduse_vq_irq_inject);
		spin_lock_init(&dev->vqs[i]->kick_lock);
		spin_lock_init(&dev->vqs[i]->irq_lock);
	}

	return 0;
err:
	for (; i >= 0; i--)
		kfree(dev->vqs[i]);
	kfree(dev->vqs);
	dev->vqs = NULL;
	return -ENOMEM;
}

static struct vduse_dev *vduse_dev_create(void)
{
	struct vduse_dev *dev = kzalloc(sizeof(*dev), GFP_KERNEL);

	if (!dev)
		return NULL;

	mutex_init(&dev->lock);
	spin_lock_init(&dev->msg_lock);
	INIT_LIST_HEAD(&dev->send_list);
	INIT_LIST_HEAD(&dev->recv_list);
	atomic64_set(&dev->msg_unique, 0);
	spin_lock_init(&dev->irq_lock);

	INIT_WORK(&dev->inject, vduse_dev_irq_inject);
	init_waitqueue_head(&dev->waitq);

	return dev;
}

static void vduse_dev_destroy(struct vduse_dev *dev)
{
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

	list_del(&dev->list);
	cdev_device_del(&dev->cdev, &dev->dev);
	put_device(&dev->dev);
	module_put(THIS_MODULE);

	return 0;
}

static void vduse_release_dev(struct device *device)
{
	struct vduse_dev *dev =
		container_of(device, struct vduse_dev, dev);

	ida_simple_remove(&vduse_ida, dev->minor);
	vduse_dev_deinit_vqs(dev);
	vduse_domain_destroy(dev->domain);
	kfree(dev->name);
	vduse_dev_destroy(dev);
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
	dev->device_id = config->device_id;
	dev->vendor_id = config->vendor_id;
	dev->name = kstrdup(config->name, GFP_KERNEL);
	if (!dev->name)
		goto err_str;

	dev->domain = vduse_domain_create(max_iova_size - 1,
					config->bounce_size);
	if (!dev->domain)
		goto err_domain;

	ret = vduse_dev_init_vqs(dev, config->vq_align,
				 config->vq_size_max, config->vq_num);
	if (ret)
		goto err_vqs;

	ret = ida_simple_get(&vduse_ida, 0, VDUSE_DEV_MAX, GFP_KERNEL);
	if (ret < 0)
		goto err_ida;

	dev->minor = ret;
	device_initialize(&dev->dev);
	dev->dev.release = vduse_release_dev;
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

	list_add(&dev->list, &vduse_devs);
	__module_get(THIS_MODULE);

	return 0;
err_ida:
	vduse_dev_deinit_vqs(dev);
err_vqs:
	vduse_domain_destroy(dev->domain);
err_domain:
	kfree(dev->name);
err_str:
	vduse_dev_destroy(dev);
	return ret;
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
	return kasprintf(GFP_KERNEL, "vduse/%s", dev_name(dev));
}

static struct miscdevice vduse_misc = {
	.fops = &vduse_fops,
	.minor = MISC_DYNAMIC_MINOR,
	.name = "vduse",
	.nodename = "vduse/control",
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
	vdev->vdpa.dev.dma_mask = &vdev->vdpa.dev.coherent_dma_mask;
	ret = dma_set_mask_and_coherent(&vdev->vdpa.dev, DMA_BIT_MASK(64));
	if (ret) {
		put_device(&vdev->vdpa.dev);
		return ret;
	}
	set_dma_ops(&vdev->vdpa.dev, &vduse_dev_dma_ops);
	vdev->vdpa.dma_dev = &vdev->vdpa.dev;
	vdev->vdpa.mdev = &mgmt_dev;

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

	ret = _vdpa_register_device(&dev->vdev->vdpa, dev->vq_num);
	if (ret) {
		put_device(&dev->vdev->vdpa.dev);
		return ret;
	}

	return 0;
}

static void vdpa_dev_del(struct vdpa_mgmt_dev *mdev, struct vdpa_device *dev)
{
	_vdpa_unregister_device(dev);
}

static const struct vdpa_mgmtdev_ops vdpa_dev_mgmtdev_ops = {
	.dev_add = vdpa_dev_add,
	.dev_del = vdpa_dev_del,
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
	vduse_domain_exit();
	vduse_mgmtdev_exit();
}
module_exit(vduse_exit);

MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE(DRV_LICENSE);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESC);
