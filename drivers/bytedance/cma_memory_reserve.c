// SPDX-License-Identifier: GPL-2.0-only
/*
 * CMA Memory Reserve Driver - Per NUMA Node
 *
 * Copyright (c) 2026, ByteDance, Inc.
 * Authors: Shen yicong shenyicong.1023@bytedance.com
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/nodemask.h>
#include <linux/gfp.h>

#define CMR_NAME "cmr_char"
#define CMR_CLASS_NAME "cmr_class"

/*
 * Module parameter for allocation size in MB.
 *
 * Default is 0, meaning do not allocate any memory and do not create devices.
 * This is useful for built-in mode where parameters are often passed via
 * kernel cmdline (e.g. cma_memory_reserve.alloc_size_mb=256).
 */
static unsigned long alloc_size_mb; /* Default 0MB */
module_param(alloc_size_mb, ulong, 0444);
MODULE_PARM_DESC(alloc_size_mb, "Size of memory to reserve per node in MB");

/* Default 0: non-coherent */
static int need_coherent;
module_param(need_coherent, int, 0444);
MODULE_PARM_DESC(need_coherent,
		 "Whether memory needs to be reserved as coherent");

struct cmr_node_ctx {
	int node_id;
	dev_t dev_num;
	struct cdev cdev;
	struct device *dev;	/* The sysfs device (/sys/devices/virtual/...) */

	/* Memory resources */
	dma_addr_t dma_handle;	/* DMA address (Physical-like address) */
	size_t size;		/* Actual size in bytes */
	void *kvaddr;		/* Kernel virtual address (Uncached) */

	struct list_head list;	/* To keep track of all contexts */
};

static struct class *cmr_class;
static LIST_HEAD(ctx_list);  /* Linked list to manage our devices */

static dev_t cmr_devt_base;
static unsigned int cmr_devt_count;

static char *cmr_devnode(const struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0600;

	/* Use default node name created by devtmpfs/udev. */
	return NULL;
}

/*
 * Helper to find context based on inode.
 * Used in open() to bind file->private_data
 */
static struct cmr_node_ctx *get_ctx_from_inode(struct inode *inode)
{
	return container_of(inode->i_cdev, struct cmr_node_ctx, cdev);
}

/*
 * READ: Read from the CMA memory to user buffer
 */
static ssize_t cmr_read(struct file *filp, char __user *buf, size_t count,
			loff_t *pos)
{
	struct cmr_node_ctx *ctx = filp->private_data;
	size_t available;
	u8 *base;

	if (!ctx || !ctx->kvaddr)
		return -EFAULT;

	base = (u8 *)ctx->kvaddr;

	if (*pos >= ctx->size)
		return 0;

	available = ctx->size - *pos;
	if (count > available)
		count = available;

	/*
	 * Note: Since kvaddr can be coherent (uncached), this copy might be slower
	 * than copying from standard RAM, but it guarantees data freshness.
	 */
	if (copy_to_user(buf, base + *pos, count))
		return -EFAULT;

	*pos += count;
	return count;
}

/*
 * WRITE: Write from user buffer to CMA memory
 */
static ssize_t cmr_write(struct file *filp, const char __user *buf,
			 size_t count, loff_t *pos)
{
	struct cmr_node_ctx *ctx = filp->private_data;
	size_t available;
	u8 *base;

	if (!ctx || !ctx->kvaddr)
		return -EFAULT;

	base = (u8 *)ctx->kvaddr;

	if (*pos >= ctx->size)
		return -ENOSPC;

	available = ctx->size - *pos;
	if (count > available)
		count = available;

	if (copy_from_user(base + *pos, buf, count))
		return -EFAULT;

	*pos += count;

	pr_debug("%s: node %d wrote %zu bytes at offset %lld\n",
		 CMR_NAME, ctx->node_id, count, *pos - count);

	return count;
}

/*
 * MMAP: Map DMA memory to user space
 *
 * - Coherent: dma_mmap_coherent()
 * - Non-coherent: dma_mmap_pages() for dma_alloc_noncoherent()
 */
static int cmr_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct cmr_node_ctx *ctx = filp->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	int ret;

	if (!ctx || !ctx->kvaddr)
		return -EFAULT;

	/*
	 * Support mapping from an offset (vma->vm_pgoff) as long as the mapping
	 * stays within the allocated buffer.
	 */
	if (offset >= ctx->size)
		return -ENXIO;

	if (size > ctx->size - offset)
		return -EINVAL;

	if (!ctx->dev)
		return -ENODEV;

	/*
	 * dma_mmap_coherent is the correct partner for dma_alloc_coherent.
	 * It maps the 'dma_handle' (physical) to the user vma, ensuring the
	 * cache attributes (Uncached) match what the kernel set up during allocation.
	 */
	if (need_coherent)
		ret = dma_mmap_coherent(ctx->dev, vma, ctx->kvaddr, ctx->dma_handle,
					ctx->size);
	else
		ret = dma_mmap_pages(ctx->dev, vma, ctx->size, virt_to_page(ctx->kvaddr));

	if (ret < 0) {
		pr_err("%s: %s failed: %d\n", CMR_NAME, __func__, ret);
		return ret;
	}

	return 0;
}

static int cmr_open(struct inode *inode, struct file *filp)
{
	struct cmr_node_ctx *ctx = get_ctx_from_inode(inode);

	if (!ctx)
		return -ENODEV;

	filp->private_data = ctx;
	return 0;
}

static int cmr_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static const struct file_operations cmr_fops = {
	.owner		= THIS_MODULE,
	.open		= cmr_open,
	.release	= cmr_release,
	.read		= cmr_read,
	.write		= cmr_write,
	.mmap		= cmr_mmap,
	.llseek		= default_llseek,
};

/*
 * Cleanup a single context (free memory, destroy device)
 */
static void cleanup_node_context(struct cmr_node_ctx *ctx)
{
	if (ctx->kvaddr) {
		if (need_coherent)
			dma_free_coherent(ctx->dev, ctx->size, ctx->kvaddr,
					  ctx->dma_handle);
		else
			dma_free_noncoherent(ctx->dev, ctx->size, ctx->kvaddr,
					     ctx->dma_handle,
					     DMA_BIDIRECTIONAL);
		pr_info("%s: node %d freed %zu bytes\n",
			CMR_NAME, ctx->node_id, ctx->size);
	}

	if (ctx->dev)
		device_destroy(cmr_class, ctx->dev_num);

	cdev_del(&ctx->cdev);

	list_del(&ctx->list);
	kfree(ctx);
}

static int __init cmr_init(void)
{
	int ret = 0;
	int node;
	size_t alloc_size_bytes;
	struct cmr_node_ctx *ctx, *tmp;

	if (!alloc_size_mb) {
		pr_info("%s: alloc_size_mb=0, skip allocation and device creation\n",
			CMR_NAME);
		return 0;
	}

	/* Calculate size */
	alloc_size_bytes = alloc_size_mb * 1024 * 1024;
	alloc_size_bytes = PAGE_ALIGN(alloc_size_bytes);

	pr_info("%s: initializing, alloc_size=%lu MB per node (%s Mode)\n",
		CMR_NAME, alloc_size_mb,
		need_coherent ? "Coherent" : "Non-Coherent");

	/* Create the device class */
	cmr_class = class_create(CMR_CLASS_NAME);
	if (IS_ERR(cmr_class)) {
		pr_err("%s: failed to create class\n", CMR_NAME);
		return PTR_ERR(cmr_class);
	}

	/* Restrict /dev/cmr_char* to root-only by default. */
	cmr_class->devnode = cmr_devnode;

	/*
	 * Allocate a single major number for the driver, and use the NUMA node id
	 * as the minor number.
	 */
	cmr_devt_count = nr_node_ids;
	ret = alloc_chrdev_region(&cmr_devt_base, 0, cmr_devt_count, CMR_NAME);
	if (ret < 0) {
		pr_err("%s: alloc_chrdev_region failed: %d\n", CMR_NAME, ret);
		class_destroy(cmr_class);
		cmr_class = NULL;
		cmr_devt_count = 0;
		return ret;
	}

	/* Iterate over memory-backed NUMA nodes */
	for_each_node_state(node, N_MEMORY) {
		dev_t devno;

		ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
		if (!ctx) {
			ret = -ENOMEM;
			goto err_cleanup;
		}

		ctx->node_id = node;
		ctx->size = alloc_size_bytes;
		INIT_LIST_HEAD(&ctx->list);

		/* 1. Use node id as minor number under a single allocated major */
		devno = MKDEV(MAJOR(cmr_devt_base), MINOR(cmr_devt_base) + node);
		ctx->dev_num = devno;

		/* 2. Create device node /dev/cmr_char{nodeid} */
		ctx->dev = device_create(cmr_class, NULL, ctx->dev_num, NULL, "%s%d",
					 CMR_NAME, node);
		if (IS_ERR(ctx->dev)) {
			ret = PTR_ERR(ctx->dev);
			pr_err("%s: device_create failed for node %d\n", CMR_NAME, node);
			kfree(ctx);
			goto err_cleanup;
		}

		/*
		 * device_create() creates a purely virtual device. Some architectures
		 * leave ->dma_mask NULL for such devices, and dma_set_mask* will return
		 * -EIO (-5). Initialize dma_mask/coherent_dma_mask explicitly.
		 */
		ctx->dev->dma_mask = &ctx->dev->coherent_dma_mask;
		ctx->dev->coherent_dma_mask = DMA_BIT_MASK(64);

		/*
		 * 4. Initialize DMA masks.
		 * Essential for dma_alloc_* to work properly.
		 */
		ret = dma_set_mask_and_coherent(ctx->dev, DMA_BIT_MASK(64));
		if (ret)
			ret = dma_set_mask_and_coherent(ctx->dev, DMA_BIT_MASK(32));
		if (ret) {
			pr_err("%s: dma_set_mask_and_coherent failed for node %d: %d\n",
			       CMR_NAME, node, ret);
			device_destroy(cmr_class, ctx->dev_num);
			kfree(ctx);
			goto err_cleanup;
		}

		/* 4. Bind device to NUMA node */
		set_dev_node(ctx->dev, node);

		/* 5. Allocate Memory using dma_alloc_coherent/dma_alloc_noncoherent */
		if (need_coherent)
			ctx->kvaddr = dma_alloc_coherent(ctx->dev,
						 ctx->size,
						 &ctx->dma_handle,
						 GFP_KERNEL);
		else
			ctx->kvaddr = dma_alloc_noncoherent(ctx->dev,
					    ctx->size,
					    &ctx->dma_handle,
					    DMA_BIDIRECTIONAL,
					    GFP_KERNEL);

		if (!ctx->kvaddr) {
			pr_err("%s: dma_alloc failed for node %d (size=%zu)\n",
			       CMR_NAME, node, ctx->size);
			ret = -ENOMEM;
			device_destroy(cmr_class, ctx->dev_num);
			kfree(ctx);
			goto err_cleanup;
		}

		/*
		 * 6. Register cdev only after memory allocation succeeds.
		 * This avoids a possible use-after-free if userspace opens the device
		 * and a later init failure triggers cleanup.
		 */
		cdev_init(&ctx->cdev, &cmr_fops);
		ctx->cdev.owner = THIS_MODULE;
		ret = cdev_add(&ctx->cdev, ctx->dev_num, 1);
		if (ret) {
			pr_err("%s: cdev_add failed for node %d\n", CMR_NAME, node);
			if (need_coherent)
				dma_free_coherent(ctx->dev, ctx->size, ctx->kvaddr,
						  ctx->dma_handle);
			else
				dma_free_noncoherent(ctx->dev, ctx->size, ctx->kvaddr,
						     ctx->dma_handle,
						     DMA_BIDIRECTIONAL);
			device_destroy(cmr_class, ctx->dev_num);
			kfree(ctx);
			goto err_cleanup;
		}

		/* Add to global list (fully initialized contexts only) */
		list_add_tail(&ctx->list, &ctx_list);

		pr_info("%s: node %d setup done. Virt: %p, DMA Addr: %llx\n",
			CMR_NAME, node, ctx->kvaddr,
			(unsigned long long)ctx->dma_handle);
	}

	return 0;

err_cleanup:
	/* Iterate list and clean up everything */
	list_for_each_entry_safe(ctx, tmp, &ctx_list, list)
		cleanup_node_context(ctx);

	if (cmr_devt_count)
		unregister_chrdev_region(cmr_devt_base, cmr_devt_count);
	cmr_devt_count = 0;
	class_destroy(cmr_class);
	return ret;
}

static void __exit cmr_exit(void)
{
	struct cmr_node_ctx *ctx, *tmp;

	pr_info("%s: unloading module\n", CMR_NAME);

	list_for_each_entry_safe(ctx, tmp, &ctx_list, list)
		cleanup_node_context(ctx);

	if (cmr_class)
		class_destroy(cmr_class);

	if (cmr_devt_count)
		unregister_chrdev_region(cmr_devt_base, cmr_devt_count);
	cmr_devt_count = 0;
}

module_init(cmr_init);
module_exit(cmr_exit);

MODULE_AUTHOR("Shen yicong <shenyicong.1023@bytedance.com>");
MODULE_DESCRIPTION("Per-NUMA CMA Memory Reserve Driver");
MODULE_LICENSE("GPL");
