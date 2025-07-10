// SPDX-License-Identifier: GPL-2.0
/*
 * Framework for HiSilicon SoC cache, manages HiSilicon SoC cache drivers.
 *
 * Copyright (c) 2024 HiSilicon Technologies Co., Ltd.
 * Author: Jie Wang <wangjie125@huawei.com>
 * Author: Yicong Yang <yangyicong@hisilicon.com>
 * Author: Yushan Wang <wangyushan12@huawei.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/memory.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pagewalk.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include <asm/page.h>

#include "hisi_soc_cache_framework.h"

struct hisi_soc_comp_inst {
	struct list_head node;
	struct hisi_soc_comp *comp;
};

struct hisi_soc_comp_list {
	struct list_head node;
	/* protects list of HiSilicon SoC cache components */
	spinlock_t lock;
	u32 inst_num;
};

static struct hisi_soc_comp_list soc_cache_devs[SOC_COMP_TYPE_MAX];

int hisi_soc_cache_maintain(phys_addr_t addr, size_t size,
			    enum hisi_soc_cache_maint_type mnt_type)
{
	struct hisi_soc_comp_inst *inst;
	struct list_head *head;
	int ret = -EOPNOTSUPP;

	if (mnt_type >= HISI_CACHE_MAINT_MAX)
		return -EINVAL;

	guard(spinlock)(&soc_cache_devs[HISI_SOC_HHA].lock);

	head = &soc_cache_devs[HISI_SOC_HHA].node;
	list_for_each_entry(inst, head, node) {
		ret = inst->comp->ops->do_maintain(inst->comp, addr, size,
						   mnt_type);
		if (ret)
			return ret;
	}

	list_for_each_entry(inst, head, node) {
		ret = inst->comp->ops->poll_maintain_done(inst->comp, addr,
							  size, mnt_type);
		if (ret)
			return ret;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(hisi_soc_cache_maintain);

static int hisi_soc_cache_maint_pte_entry(pte_t *pte, unsigned long addr,
				unsigned long next, struct mm_walk *walk)
{
#ifdef HISI_SOC_CACHE_LLT
	unsigned int mnt_type = *((unsigned int *)walk->priv);
#else
	unsigned int mnt_type = *((unsigned int *)walk->private);
#endif
	size_t size = next - addr;
	phys_addr_t paddr;

	if (!pte_present(ptep_get(pte)))
		return -EINVAL;

	paddr = PFN_PHYS(pte_pfn(*pte)) + offset_in_page(addr);

	return hisi_soc_cache_maintain(paddr, size, mnt_type);
}

static const struct mm_walk_ops hisi_soc_cache_maint_walk = {
	.pte_entry = hisi_soc_cache_maint_pte_entry,
	.walk_lock = PGWALK_RDLOCK,
};

static int hisi_soc_cache_inst_check(const struct hisi_soc_comp *comp,
				     enum hisi_soc_comp_type comp_type)
{
	/* Different types of component could have different ops. */
	switch (comp_type) {
	case HISI_SOC_HHA:
		if (!comp->ops->do_maintain || !comp->ops->poll_maintain_done)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int hisi_soc_cache_inst_add(struct hisi_soc_comp *comp,
				   enum hisi_soc_comp_type comp_type)
{
	struct hisi_soc_comp_inst *comp_inst;
	int ret;

	ret = hisi_soc_cache_inst_check(comp, comp_type);
	if (ret)
		return ret;

	comp_inst = kzalloc(sizeof(*comp_inst), GFP_KERNEL);
	if (!comp_inst)
		return -ENOMEM;

	comp_inst->comp = comp;

	scoped_guard(spinlock, &soc_cache_devs[comp_type].lock) {
		list_add_tail(&comp_inst->node,
			      &soc_cache_devs[comp_type].node);
		soc_cache_devs[comp_type].inst_num++;
	}

	return 0;
}

/*
 * When @comp is NULL, it means to delete all instances of @comp_type.
 */
static void hisi_soc_cache_inst_del(struct hisi_soc_comp *comp,
				    enum hisi_soc_comp_type comp_type)
{
	struct hisi_soc_comp_inst *inst, *tmp;

	guard(spinlock)(&soc_cache_devs[comp_type].lock);
	list_for_each_entry_safe(inst, tmp, &soc_cache_devs[comp_type].node,
				 node) {
		if (comp && comp != inst->comp)
			continue;

		if (soc_cache_devs[comp_type].inst_num > 0)
			soc_cache_devs[comp_type].inst_num--;

		list_del(&inst->node);
		kfree(inst);

		/* Stop the loop if we have already deleted @comp. */
		if (comp)
			break;
	}
}

int hisi_soc_comp_inst_add(struct hisi_soc_comp *comp)
{
	int ret, i = 0;

	if (!comp || !comp->ops || comp->comp_type == 0)
		return -EINVAL;

	for_each_set_bit_from(i, &comp->comp_type, SOC_COMP_TYPE_MAX) {
		ret = hisi_soc_cache_inst_add(comp, i);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(hisi_soc_comp_inst_add);

int hisi_soc_comp_inst_del(struct hisi_soc_comp *comp)
{
	int i;

	if (!comp)
		return -EINVAL;

	for_each_set_bit(i, &comp->comp_type, SOC_COMP_TYPE_MAX)
		hisi_soc_cache_inst_del(comp, i);

	return 0;
}
EXPORT_SYMBOL_GPL(hisi_soc_comp_inst_del);

static int __hisi_soc_cache_maintain(unsigned long __user vaddr, size_t size,
				     enum hisi_soc_cache_maint_type mnt_type)
{
	unsigned long start = untagged_addr(vaddr);
	struct vm_area_struct *vma;
	int ret = 0;

	mmap_read_lock_killable(current->mm);

	vma = vma_lookup(current->mm, vaddr);
	if (!vma || vaddr + size > vma->vm_end || !size) {
		ret = -EINVAL;
		goto out;
	}

	/* User should have the write permission of target memory */
	if (!(vma->vm_flags & VM_WRITE)) {
		ret = -EINVAL;
		goto out;
	}

	ret = walk_page_range(current->mm, start, start + size,
			&hisi_soc_cache_maint_walk, &mnt_type);

out:
	mmap_read_unlock(current->mm);
	return ret;
}

static long hisi_soc_cache_mgmt_ioctl(struct file *file, u32 cmd, unsigned long arg)
{
	struct hisi_soc_cache_ioctl_param *param =
		kzalloc(sizeof(struct hisi_soc_cache_ioctl_param), GFP_KERNEL);
	long ret;

	if (!param) {
		ret = -ENOMEM;
		goto out;
	}

	if (copy_from_user(param, (void __user *)arg, sizeof(*param))) {
		ret = -EFAULT;
		goto out;
	}

	switch (cmd) {
	case HISI_CACHE_MAINTAIN:
		ret = __hisi_soc_cache_maintain(param->addr, param->size,
						param->op_type);
		break;
	default:
		ret = -EINVAL;
		break;
	}
out:
	kfree(param);
	return ret;
}

static const struct file_operations soc_cache_dev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = hisi_soc_cache_mgmt_ioctl,
};

static struct miscdevice soc_cache_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "hisi_soc_cache_mgmt",
	.fops = &soc_cache_dev_fops,
	.mode = 0600,
};

static void hisi_soc_cache_inst_uninit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(soc_cache_devs); ++i)
		hisi_soc_cache_inst_del(NULL, i);
}

static void hisi_soc_cache_framework_data_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(soc_cache_devs); ++i) {
		spin_lock_init(&soc_cache_devs[i].lock);
		INIT_LIST_HEAD(&soc_cache_devs[i].node);
	}
}

static const char *const hisi_soc_cache_item_str[SOC_COMP_TYPE_MAX] = {
	"hha"
};

/*
 * Print cache instance number debug information for debug FS.
 */
static ssize_t hisi_soc_cache_dbg_get_inst_num(struct file *file,
					       char __user *buff,
					       size_t cnt,
					       loff_t *ppos)
{
#define HISI_SOC_CACHE_DBGFS_REG_LEN 100
	char *read_buff;
	int len, i, pos = 0;
	int ret = 0;

	if (!access_ok(buff, cnt))
		return -EFAULT;
	if (*ppos < 0)
		return -EINVAL;
	if (cnt == 0)
		return 0;

	read_buff = kzalloc(HISI_SOC_CACHE_DBGFS_REG_LEN, GFP_KERNEL);
	if (!read_buff)
		return -ENOMEM;

	len = HISI_SOC_CACHE_DBGFS_REG_LEN;

	for (i = 0; i < ARRAY_SIZE(soc_cache_devs); i++) {
		guard(spinlock)(&soc_cache_devs[i].lock);
		pos += scnprintf(read_buff + pos, len - pos,
				 "%s inst num: %u\n",
				 hisi_soc_cache_item_str[i],
				 soc_cache_devs[i].inst_num);
	}

	ret = simple_read_from_buffer(buff, cnt, ppos, read_buff,
				       strlen(read_buff));
	kfree(read_buff);
	return ret;
}

static struct dentry *hisi_cache_dbgfs_root;
static const struct file_operations hisi_cache_dbgfs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = hisi_soc_cache_dbg_get_inst_num,
};

static void hisi_soc_cache_dbgfs_init(void)
{
	hisi_cache_dbgfs_root = debugfs_create_dir("hisi_soc_cache_frm", NULL);
	debugfs_create_file("instance", 0400, hisi_cache_dbgfs_root, NULL,
			    &hisi_cache_dbgfs_ops);
}

static void hisi_soc_cache_dbgfs_uninit(void)
{
	debugfs_remove_recursive(hisi_cache_dbgfs_root);
	hisi_cache_dbgfs_root = NULL;
}

static int __init hisi_soc_cache_framework_init(void)
{
	int ret;

	hisi_soc_cache_framework_data_init();

	ret = misc_register(&soc_cache_miscdev);
	if (ret) {
		hisi_soc_cache_inst_uninit();
		return ret;
	}

	hisi_soc_cache_dbgfs_init();

	return 0;
}
module_init(hisi_soc_cache_framework_init);

static void __exit hisi_soc_cache_framework_exit(void)
{
	hisi_soc_cache_dbgfs_uninit();
	misc_deregister(&soc_cache_miscdev);
	hisi_soc_cache_inst_uninit();
}
module_exit(hisi_soc_cache_framework_exit);

MODULE_DESCRIPTION("HiSilicon SoC Cache Framework Driver");
MODULE_AUTHOR("Jie Wang <wangjie125@huawei.com>");
MODULE_AUTHOR("Yushan Wang <wangyushan12@huawei.com>");
MODULE_LICENSE("GPL");
