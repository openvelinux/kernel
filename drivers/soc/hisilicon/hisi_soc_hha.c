// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for HiSilicon Hydra Home Agent (HHA).
 *
 * Copyright (c) 2024 HiSilicon Technologies Co., Ltd.
 * Author: Yicong Yang <yangyicong@hisilicon.com>
 *         Yushan Wang <wangyushan12@huawei.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitfield.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include "hisi_soc_cache_framework.h"

#define HISI_HHA_CTRL		0x5004
#define   HISI_HHA_CTRL_EN	BIT(0)
#define   HISI_HHA_CTRL_RANGE	BIT(1)
#define   HISI_HHA_CTRL_TYPE	GENMASK(3, 2)
#define HISI_HHA_START_L	0x5008
#define HISI_HHA_START_H	0x500c
#define HISI_HHA_LEN_L		0x5010
#define HISI_HHA_LEN_H		0x5014

/* The maintain operation performs in a 128 Byte granularity */
#define HISI_HHA_MAINT_ALIGN	128

#define HISI_HHA_POLL_GAP_US		10

struct hisi_soc_hha {
	struct hisi_soc_comp comp;
	/* Locks HHA instance to forbid overlapping access. */
	spinlock_t lock;
	struct device *dev;
	void __iomem *base;
};

static bool hisi_hha_cache_maintain_wait_finished(struct hisi_soc_hha *soc_hha)
{
	u32 val;

	return !readl_poll_timeout_atomic(soc_hha->base + HISI_HHA_CTRL, val,
					  !(val & HISI_HHA_CTRL_EN),
					  HISI_HHA_POLL_GAP_US,
					  jiffies_to_usecs(HZ));
}

static int hisi_hha_cache_do_maintain(struct hisi_soc_comp *comp,
				      phys_addr_t addr, size_t size,
				      enum hisi_soc_cache_maint_type mnt_type)
{
	struct hisi_soc_hha *soc_hha = container_of(comp, struct hisi_soc_hha,
						    comp);
	int ret = 0;
	u32 reg;

	if (!size)
		return -EINVAL;

	if (mnt_type < 0)
		return -EOPNOTSUPP;

	/*
	 * Hardware will search for addresses ranging [addr, addr + size -1],
	 * last byte included, and perform maintain in 128 byte granule
	 * on those which contain the addresses.
	 */
	size -= 1;

	guard(spinlock)(&soc_hha->lock);

	if (!hisi_hha_cache_maintain_wait_finished(soc_hha))
		return -EBUSY;

	writel(lower_32_bits(addr), soc_hha->base + HISI_HHA_START_L);
	writel(upper_32_bits(addr), soc_hha->base + HISI_HHA_START_H);
	writel(lower_32_bits(size), soc_hha->base + HISI_HHA_LEN_L);
	writel(upper_32_bits(size), soc_hha->base + HISI_HHA_LEN_H);

	reg = FIELD_PREP(HISI_HHA_CTRL_TYPE, mnt_type);
	reg |= HISI_HHA_CTRL_RANGE | HISI_HHA_CTRL_EN;
	writel(reg, soc_hha->base + HISI_HHA_CTRL);

	return ret;
}

static int hisi_hha_cache_poll_maintain_done(struct hisi_soc_comp *comp,
					     phys_addr_t addr, size_t size,
					     enum hisi_soc_cache_maint_type mnt_type)
{
	struct hisi_soc_hha *soc_hha = container_of(comp, struct hisi_soc_hha,
						    comp);

	guard(spinlock)(&soc_hha->lock);

	if (!hisi_hha_cache_maintain_wait_finished(soc_hha))
		return -ETIMEDOUT;

	return 0;
}

static struct hisi_soc_comp_ops hisi_soc_hha_comp_ops = {
	.do_maintain = hisi_hha_cache_do_maintain,
	.poll_maintain_done = hisi_hha_cache_poll_maintain_done,
};

static void hisi_hha_comp_inst_del(void *priv)
{
	struct hisi_soc_hha *soc_hha = priv;

	hisi_soc_comp_inst_del(&soc_hha->comp);
}

static int hisi_soc_hha_probe(struct platform_device *pdev)
{
	struct hisi_soc_hha *soc_hha;
	struct resource *mem;
	int ret;

	soc_hha = devm_kzalloc(&pdev->dev, sizeof(*soc_hha), GFP_KERNEL);
	if (!soc_hha)
		return -ENOMEM;

	platform_set_drvdata(pdev, soc_hha);
	soc_hha->dev = &pdev->dev;

	spin_lock_init(&soc_hha->lock);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -ENODEV;

	/*
	 * HHA cache driver share the same register region with HHA uncore PMU
	 * driver in hardware's perspective, none of them should reserve the
	 * resource to itself only.  Here exclusive access verification is
	 * avoided by calling devm_ioremap instead of devm_ioremap_resource to
	 * allow both drivers to exist at the same time.
	 */
	soc_hha->base = devm_ioremap(&pdev->dev, mem->start,
				     resource_size(mem));
	if (IS_ERR_OR_NULL(soc_hha->base)) {
		return dev_err_probe(&pdev->dev, PTR_ERR(soc_hha->base),
				     "failed to remap io memory");
	}

	soc_hha->comp.ops = &hisi_soc_hha_comp_ops;
	soc_hha->comp.comp_type = BIT(HISI_SOC_HHA);
	cpumask_copy(&soc_hha->comp.affinity_mask, cpu_possible_mask);

	ret = hisi_soc_comp_inst_add(&soc_hha->comp);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register maintain inst");

	return devm_add_action_or_reset(&pdev->dev, hisi_hha_comp_inst_del,
					soc_hha);
}

static const struct acpi_device_id hisi_soc_hha_ids[] = {
	{ "HISI0511", },
	{ }
};
MODULE_DEVICE_TABLE(acpi, hisi_soc_hha_ids);

static struct platform_driver hisi_soc_hha_driver = {
	.driver = {
		.name = "hisi_soc_hha",
		.acpi_match_table = hisi_soc_hha_ids,
	},
	.probe = hisi_soc_hha_probe,
};

module_platform_driver(hisi_soc_hha_driver);

MODULE_DESCRIPTION("Hisilicon Hydra Home Agent driver supporting cache maintenance");
MODULE_AUTHOR("Yicong Yang <yangyicong@hisilicon.com>");
MODULE_AUTHOR("Yushan Wang <wangyushan12@huawei.com>");
MODULE_LICENSE("GPL");
