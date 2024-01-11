// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019-2020 HUAWEI TECHNOLOGIES CO., LTD., All Rights Reserved.
 * Author: Wanghaibin <wanghaibin.wang@huawei.com>
 */

#include <linux/irq.h>
#include <linux/kvm.h>
#include <linux/kvm_host.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

static struct workqueue_struct *sdev_cleanup_wq;
static bool virt_msi_bypass;
bool sdev_enable;

static void shadow_dev_destroy(struct work_struct *work);
static void sdev_virt_pdev_delete(struct platform_device *pdev);

int shadow_dev_virq_bypass_inject(struct kvm *kvm,
				  struct kvm_kernel_irq_routing_entry *e)
{
	struct shadow_dev *sdev = e->cache.data;
	u32 vec = e->msi.data;
	u32 host_irq = sdev->host_irq[vec];
	int ret;

	ret = irq_set_irqchip_state(host_irq, IRQCHIP_STATE_PENDING, true);
	WARN_RATELIMIT(ret, "IRQ %d", host_irq);

	return ret;
}

/* Must be called with the dist->sdev_list_lock held */
struct shadow_dev *kvm_shadow_dev_get(struct kvm *kvm, struct kvm_msi *msi)
{
	struct vgic_dist *dist = &kvm->arch.vgic;
	struct shadow_dev *sdev;

	if (!sdev_enable)
		return NULL;

	list_for_each_entry(sdev, &dist->sdev_list_head, entry) {
		if (sdev->devid != msi->devid)
			continue;

		if (sdev->nvecs <= msi->data ||
		    !test_bit(msi->data, sdev->enable))
			break;

		return sdev;
	}

	return NULL;
}

static struct platform_device *sdev_virt_pdev_add(u32 nvec)
{
	struct platform_device *virtdev;
	int ret = -ENOMEM;

	virtdev = platform_device_alloc("virt_plat_dev", PLATFORM_DEVID_AUTO);
	if (!virtdev) {
		kvm_err("Allocate virtual platform device failed\n");
		goto out;
	}

	dev_set_drvdata(&virtdev->dev, &nvec);

	ret = platform_device_add(virtdev);
	if (ret) {
		kvm_err("Add virtual platform device failed (%d)\n", ret);
		goto put_device;
	}

	return virtdev;

put_device:
	platform_device_put(virtdev);
out:
	return ERR_PTR(ret);
}

static void sdev_set_irq_entry(struct shadow_dev *sdev,
			       struct kvm_kernel_irq_routing_entry *irq_entries)
{
	int i;

	for (i = 0; i < sdev->nvecs; i++) {
		irq_entries[i].msi.address_lo = sdev->msi[i].address_lo;
		irq_entries[i].msi.address_hi = sdev->msi[i].address_hi;
		irq_entries[i].msi.data = sdev->msi[i].data;
		irq_entries[i].msi.flags = sdev->msi[i].flags;
		irq_entries[i].msi.devid = sdev->msi[i].devid;
	}
}

static int sdev_virq_bypass_active(struct kvm *kvm, struct shadow_dev *sdev)
{
	struct kvm_kernel_irq_routing_entry *irq_entries;
	struct msi_desc *desc;
	u32 vec = 0;

	sdev->host_irq = kcalloc(sdev->nvecs, sizeof(int), GFP_KERNEL);
	sdev->enable   = bitmap_zalloc(sdev->nvecs, GFP_KERNEL);
	irq_entries    = kcalloc(sdev->nvecs,
				 sizeof(struct kvm_kernel_irq_routing_entry),
				 GFP_KERNEL);

	if (!irq_entries || !sdev->enable || !sdev->host_irq) {
		kfree(sdev->host_irq);
		kfree(sdev->enable);
		kfree(irq_entries);
		return -ENOMEM;
	}

	sdev_set_irq_entry(sdev, irq_entries);

	msi_for_each_desc(desc, &sdev->pdev->dev, MSI_DESC_ALL) {
		if (!kvm_vgic_v4_set_forwarding(kvm, desc->irq,
						&irq_entries[vec])) {
			set_bit(vec, sdev->enable);
			sdev->host_irq[vec] = desc->irq;
		} else {
			/*
			 * Can not use shadow device for direct injection,
			 * though not fatal...
			 */
			kvm_err("Shadow device set (%d) forwarding failed",
				desc->irq);
		}
		vec++;
	}

	kfree(irq_entries);
	return 0;
}

static void sdev_msi_entry_init(struct kvm_master_dev_info *mdi,
				struct shadow_dev *sdev)
{
	int i;

	for (i = 0; i < sdev->nvecs; i++) {
		sdev->msi[i].address_lo = mdi->msi[i].address_lo;
		sdev->msi[i].address_hi = mdi->msi[i].address_hi;
		sdev->msi[i].data = mdi->msi[i].data;
		sdev->msi[i].flags = mdi->msi[i].flags;
		sdev->msi[i].devid = mdi->msi[i].devid;
	}
}

int kvm_shadow_dev_create(struct kvm *kvm, struct kvm_master_dev_info *mdi)
{
	struct vgic_dist *dist = &kvm->arch.vgic;
	struct shadow_dev *sdev;
	struct kvm_msi *msi;
	unsigned long flags;
	int ret;

	if (WARN_ON(!sdev_enable))
		return -EINVAL;

	ret = -ENOMEM;
	sdev = kzalloc(sizeof(struct shadow_dev), GFP_KERNEL);
	if (!sdev)
		return ret;

	sdev->nvecs = mdi->nvectors;

	msi = kcalloc(sdev->nvecs, sizeof(struct kvm_msi), GFP_KERNEL);
	if (!msi)
		goto free_sdev;

	sdev->msi = msi;
	sdev_msi_entry_init(mdi, sdev);
	sdev->devid = sdev->msi[0].devid;

	sdev->pdev = sdev_virt_pdev_add(sdev->nvecs);
	if (IS_ERR(sdev->pdev)) {
		ret = PTR_ERR(sdev->pdev);
		goto free_sdev_msi;
	}

	ret = sdev_virq_bypass_active(kvm, sdev);
	if (ret)
		goto delete_virtdev;

	sdev->kvm = kvm;
	INIT_WORK(&sdev->destroy, shadow_dev_destroy);

	raw_spin_lock_irqsave(&dist->sdev_list_lock, flags);
	list_add_tail(&sdev->entry, &dist->sdev_list_head);
	raw_spin_unlock_irqrestore(&dist->sdev_list_lock, flags);

	kvm_info("Create shadow device: 0x%x\n", sdev->devid);
	return ret;

delete_virtdev:
	sdev_virt_pdev_delete(sdev->pdev);
free_sdev_msi:
	kfree(sdev->msi);
free_sdev:
	kfree(sdev);
	return ret;
}

static void sdev_virt_pdev_delete(struct platform_device *pdev)
{
	platform_device_unregister(pdev);
}

static void sdev_virq_bypass_deactive(struct kvm *kvm, struct shadow_dev *sdev)
{
	struct kvm_kernel_irq_routing_entry *irq_entries;
	struct msi_desc *desc;
	u32 vec = 0;

	irq_entries = kcalloc(sdev->nvecs,
			      sizeof(struct kvm_kernel_irq_routing_entry),
			      GFP_KERNEL);
	if (!irq_entries)
		return;

	sdev_set_irq_entry(sdev, irq_entries);

	msi_for_each_desc(desc, &sdev->pdev->dev, MSI_DESC_ALL) {
		if (!kvm_vgic_v4_unset_forwarding(kvm, desc->irq,
						  &irq_entries[vec])) {
			clear_bit(vec, sdev->enable);
			sdev->host_irq[vec] = 0;
		} else {
			kvm_err("Shadow device unset (%d) forwarding failed",
				desc->irq);
		}
		vec++;
	}

	kfree(sdev->host_irq);
	kfree(sdev->enable);
	kfree(irq_entries);

	/* FIXME: no error handling */
}

static void shadow_dev_destroy(struct work_struct *work)
{
	struct shadow_dev *sdev = container_of(work, struct shadow_dev, destroy);
	struct kvm *kvm = sdev->kvm;

	sdev_virq_bypass_deactive(kvm, sdev);
	sdev_virt_pdev_delete(sdev->pdev);

	sdev->nvecs = 0;
	kfree(sdev->msi);
	kfree(sdev);
}

void kvm_shadow_dev_delete(struct kvm *kvm, u32 devid)
{
	struct vgic_dist *dist = &kvm->arch.vgic;
	struct shadow_dev *sdev, *tmp;
	unsigned long flags;

	if (WARN_ON(!sdev_enable))
		return;

	raw_spin_lock_irqsave(&dist->sdev_list_lock, flags);
	WARN_ON(list_empty(&dist->sdev_list_head)); /* shouldn't be invoked */

	list_for_each_entry_safe(sdev, tmp, &dist->sdev_list_head, entry) {
		if (sdev->devid != devid)
			continue;

		list_del(&sdev->entry);
		queue_work(sdev_cleanup_wq, &sdev->destroy);
		break;
	}
	raw_spin_unlock_irqrestore(&dist->sdev_list_lock, flags);

	flush_workqueue(sdev_cleanup_wq);
}

void kvm_shadow_dev_delete_all(struct kvm *kvm)
{
	struct vgic_dist *dist = &kvm->arch.vgic;
	struct shadow_dev *sdev, *tmp;
	unsigned long flags;

	if (!sdev_enable)
		return;

	raw_spin_lock_irqsave(&dist->sdev_list_lock, flags);

	list_for_each_entry_safe(sdev, tmp, &dist->sdev_list_head, entry) {
		list_del(&sdev->entry);
		queue_work(sdev_cleanup_wq, &sdev->destroy);
	}

	raw_spin_unlock_irqrestore(&dist->sdev_list_lock, flags);

	flush_workqueue(sdev_cleanup_wq);
}

static int __init early_virt_msi_bypass(char *buf)
{
	return strtobool(buf, &virt_msi_bypass);
}
early_param("kvm-arm.virt_msi_bypass", early_virt_msi_bypass);

void kvm_shadow_dev_init(void)
{
	/*
	 * FIXME: Ideally shadow device should only rely on a GICv4.0
	 * capable ITS, but we should also take the reserved device ID
	 * pools into account.
	 */
	sdev_enable = kvm_vgic_global_state.has_gicv4 && virt_msi_bypass;

	sdev_cleanup_wq = alloc_workqueue("kvm-sdev-cleanup", 0, 0);
	if (!sdev_cleanup_wq)
		sdev_enable = false;

	kvm_info("Shadow device %sabled\n", sdev_enable ? "en" : "dis");
}
