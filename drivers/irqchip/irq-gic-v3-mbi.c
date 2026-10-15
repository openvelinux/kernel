// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 ARM Limited, All Rights Reserved.
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 */

#define pr_fmt(fmt) "GICv3: " fmt

#include <linux/acpi.h>
#include <linux/acpi_iort.h>
#include <linux/iommu.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/msi.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <linux/irqchip/arm-gic-v3.h>

#include "irq-gic-common.h"

struct mbi_range {
	u32			spi_start;
	u32			nr_spis;
	unsigned long		*bm;
};

static DEFINE_MUTEX(mbi_lock);
static phys_addr_t		mbi_phys_base;
static struct mbi_range		*mbi_ranges;
static unsigned int		mbi_range_nr;

struct mbi_fw_config {
	phys_addr_t		base;
	struct gic_mbi_range	*ranges;
	unsigned int		nr_ranges;
};

static struct mbi_fw_config	mbi_acpi_cfg;

static phys_addr_t mbi_get_frame_address(struct irq_data *d)
{
	if (acpi_disabled) {
		return mbi_phys_base;
	} else {
		return gic_get_mbi_frame_address(d);
	}
}

static int mbi_get_frames(phys_addr_t *mbi_phys_addr)
{
	if (acpi_disabled) {
		*mbi_phys_addr = mbi_phys_base;
		return 1;
	} else {
		return gic_get_mbi_frames(mbi_phys_addr);
	}
}

static int mbi_set_affinity(struct irq_data *d, const struct cpumask *mask_val,
			    bool force);

static struct irq_chip mbi_irq_chip = {
	.name			= "MBI",
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_type		= irq_chip_set_type_parent,
	.irq_set_affinity	= mbi_set_affinity,
};

static int mbi_irq_gic_domain_alloc(struct irq_domain *domain,
				       unsigned int virq,
				       irq_hw_number_t hwirq)
{
	struct irq_fwspec fwspec;
	struct irq_data *d;
	int err;

	if (is_of_node(domain->parent->fwnode)) {
		fwspec.fwnode = domain->parent->fwnode;
		fwspec.param_count = 3;
		fwspec.param[0] = 0;
		fwspec.param[1] = hwirq - 32;
		fwspec.param[2] = IRQ_TYPE_EDGE_RISING;
	} else if (is_fwnode_irqchip(domain->parent->fwnode)) {
		fwspec.fwnode = domain->parent->fwnode;
		fwspec.param_count = 2;
		fwspec.param[0] = hwirq;
		fwspec.param[1] = IRQ_TYPE_EDGE_RISING;
	} else {
		return -EINVAL;
	}

	err = irq_domain_alloc_irqs_parent(domain, virq, 1, &fwspec);
	if (err)
		return err;

	d = irq_domain_get_irq_data(domain->parent, virq);
	return d->chip->irq_set_type(d, IRQ_TYPE_EDGE_RISING);
}

static void mbi_free_msi(struct mbi_range *mbi, unsigned int hwirq,
			 int nr_irqs)
{
	mutex_lock(&mbi_lock);
	bitmap_release_region(mbi->bm, hwirq - mbi->spi_start,
			      get_count_order(nr_irqs));
	mutex_unlock(&mbi_lock);
}

static int mbi_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				   unsigned int nr_irqs, void *args)
{
	msi_alloc_info_t *info = args;
	struct mbi_range *mbi = NULL;
	int frame_cnt, hwirq, offset, i, err = 0;
	phys_addr_t mbi_phys_addr[GIC_MAX_MBI_FRAMES];

	mutex_lock(&mbi_lock);
	for (i = 0; i < mbi_range_nr; i++) {
		offset = bitmap_find_free_region(mbi_ranges[i].bm,
						 mbi_ranges[i].nr_spis,
						 get_count_order(nr_irqs));
		if (offset >= 0) {
			mbi = &mbi_ranges[i];
			break;
		}
	}
	mutex_unlock(&mbi_lock);

	if (!mbi)
		return -ENOSPC;

	hwirq = mbi->spi_start + offset;
	frame_cnt = mbi_get_frames(mbi_phys_addr);
	if (!frame_cnt)
		return -ENODEV;

	for (i = 0; i < frame_cnt; i++) {
		pr_debug("%s mapping %pa\n", __func__, &mbi_phys_addr[i]);
		err = iommu_dma_prepare_msi(info->desc, mbi_phys_addr[i] + GICD_SETSPI_NSR);
		if (err)
			return err;
	}

	for (i = 0; i < nr_irqs; i++) {
		struct irq_data *irqd;

		err = mbi_irq_gic_domain_alloc(domain, virq + i, hwirq + i);
		if (err)
			goto fail;

		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i, &mbi_irq_chip, mbi);
		irqd = irq_get_irq_data(virq + i);
		irqd_set_resend_when_in_progress(irqd);
	}

	return 0;

fail:
	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
	mbi_free_msi(mbi, hwirq, nr_irqs);
	return err;
}

static void mbi_irq_domain_free(struct irq_domain *domain,
				unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct mbi_range *mbi = irq_data_get_irq_chip_data(d);

	mbi_free_msi(mbi, d->hwirq, nr_irqs);
	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
}

static const struct irq_domain_ops mbi_domain_ops = {
	.alloc			= mbi_irq_domain_alloc,
	.free			= mbi_irq_domain_free,
};

int mbi_set_acpi_data(const struct gic_mbi_range *ranges, unsigned int nr_ranges)
{
	kfree(mbi_acpi_cfg.ranges);
	mbi_acpi_cfg.ranges = NULL;
	mbi_acpi_cfg.nr_ranges = 0;

	if (!nr_ranges)
		return 0;

	mbi_acpi_cfg.ranges = kmemdup(ranges,
				      array_size(nr_ranges,
						 sizeof(*mbi_acpi_cfg.ranges)),
				      GFP_KERNEL);
	if (!mbi_acpi_cfg.ranges) {
		pr_err("MBI ACPI: kmemdup ranges failed (nr=%u size=%zu)\n",
		       nr_ranges,
		       array_size(nr_ranges, sizeof(*mbi_acpi_cfg.ranges)));
		return -ENOMEM;
	}

	mbi_acpi_cfg.nr_ranges = nr_ranges;
	return 0;
}

static int mbi_set_affinity(struct irq_data *d, const struct cpumask *mask_val,
			    bool force)
{
	return gic_mbi_set_affinity(d, mask_val, force);
}

static void mbi_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	mbi_phys_base = mbi_get_frame_address(data);
	msg[0].data = data->parent_data->hwirq;
	msi_msg_set_addr(irq_data_get_msi_desc(data), &msg[0], mbi_phys_base + GICD_SETSPI_NSR);
}

#ifdef CONFIG_PCI_MSI
/* PCI-specific irqchip */
static void mbi_mask_msi_irq(struct irq_data *d)
{
	pci_msi_mask_irq(d);
	irq_chip_mask_parent(d);
}

static void mbi_unmask_msi_irq(struct irq_data *d)
{
	pci_msi_unmask_irq(d);
	irq_chip_unmask_parent(d);
}

static struct irq_chip mbi_msi_irq_chip = {
	.name			= "MSI",
	.irq_mask		= mbi_mask_msi_irq,
	.irq_unmask		= mbi_unmask_msi_irq,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_compose_msi_msg	= mbi_compose_msi_msg,
};

static struct msi_domain_info mbi_msi_domain_info = {
	.flags	= (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		   MSI_FLAG_PCI_MSIX | MSI_FLAG_MULTI_PCI_MSI),
	.chip	= &mbi_msi_irq_chip,
};

static int mbi_allocate_pci_domain(struct irq_domain *nexus_domain,
				   struct irq_domain **pci_domain)
{
	*pci_domain = pci_msi_create_irq_domain(nexus_domain->fwnode,
						&mbi_msi_domain_info,
						nexus_domain);
	if (!*pci_domain)
		return -ENOMEM;

	return 0;
}
#else
static int mbi_allocate_pci_domain(struct irq_domain *nexus_domain,
				   struct irq_domain **pci_domain)
{
	*pci_domain = NULL;
	return 0;
}
#endif

static void mbi_compose_mbi_msg(struct irq_data *data, struct msi_msg *msg)
{
	mbi_phys_base = mbi_get_frame_address(data);

	mbi_compose_msi_msg(data, msg);

	msg[1].data = data->parent_data->hwirq;
	msi_msg_set_addr(irq_data_get_msi_desc(data), &msg[1], mbi_phys_base + GICD_CLRSPI_NSR);
}

/* Platform-MSI specific irqchip */
static struct irq_chip mbi_pmsi_irq_chip = {
	.name			= "pMSI",
	.irq_set_type		= irq_chip_set_type_parent,
	.irq_compose_msi_msg	= mbi_compose_mbi_msg,
	.flags			= IRQCHIP_SUPPORTS_LEVEL_MSI,
};

static struct msi_domain_ops mbi_pmsi_ops = {
};

static struct msi_domain_info mbi_pmsi_domain_info = {
	.flags	= (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		   MSI_FLAG_LEVEL_CAPABLE),
	.ops	= &mbi_pmsi_ops,
	.chip	= &mbi_pmsi_irq_chip,
};

static int mbi_allocate_domains(struct irq_domain *parent,
				struct fwnode_handle *fwnode)
{
	struct irq_domain *nexus_domain, *pci_domain, *plat_domain;
	int err;

	nexus_domain = irq_domain_create_hierarchy(parent, 0, 0, fwnode,
						   &mbi_domain_ops, NULL);
	if (!nexus_domain)
		return -ENOMEM;

	irq_domain_update_bus_token(nexus_domain, DOMAIN_BUS_NEXUS);

	err = mbi_allocate_pci_domain(nexus_domain, &pci_domain);

	plat_domain = platform_msi_create_irq_domain(fwnode,
						     &mbi_pmsi_domain_info,
						     nexus_domain);

	if (err || !plat_domain) {
		if (plat_domain)
			irq_domain_remove(plat_domain);
		if (pci_domain)
			irq_domain_remove(pci_domain);
		irq_domain_remove(nexus_domain);
		return -ENOMEM;
	}

	return 0;
}

int __init mbi_init(struct fwnode_handle *fwnode, struct irq_domain *parent)
{
	struct device_node *np;
	const __be32 *reg;
	int ret = 0, n;
	const char *err_msg = "unknown";
	struct fwnode_handle *mbi_fwnode = NULL;

	if (is_of_node(fwnode)) {
		np = to_of_node(fwnode);

		if (!of_property_read_bool(np, "msi-controller"))
			return 0;

		n = of_property_count_elems_of_size(np, "mbi-ranges", sizeof(u32));
		if (n <= 0 || n % 2)
			return -EINVAL;

		mbi_range_nr = n / 2;
		mbi_ranges = kcalloc(mbi_range_nr, sizeof(*mbi_ranges),
				GFP_KERNEL);
		if (!mbi_ranges) {
			pr_err("MBI: kcalloc mbi_ranges failed (nr=%u size=%zu)\n",
					mbi_range_nr,
					array_size(mbi_range_nr, sizeof(*mbi_ranges)));
			return -ENOMEM;
		}

		for (n = 0; n < mbi_range_nr; n++) {
			ret = of_property_read_u32_index(np, "mbi-ranges",
					n * 2,
					&mbi_ranges[n].spi_start);
			if (ret)
				goto err_free_mbi;
			ret = of_property_read_u32_index(np, "mbi-ranges",
					n * 2 + 1,
					&mbi_ranges[n].nr_spis);
			if (ret)
				goto err_free_mbi;

			mbi_ranges[n].bm = bitmap_zalloc(mbi_ranges[n].nr_spis,
					GFP_KERNEL);
			if (!mbi_ranges[n].bm) {
				pr_err("MBI: bitmap_zalloc failed (nr_spis=%u)\n",
						mbi_ranges[n].nr_spis);
				ret = -ENOMEM;
				goto err_free_mbi;
			}
			pr_info("MBI range [%d:%d]\n", mbi_ranges[n].spi_start,
					mbi_ranges[n].spi_start + mbi_ranges[n].nr_spis - 1);
		}

		reg = of_get_property(np, "mbi-alias", NULL);
		if (reg) {
			mbi_phys_base = of_translate_address(np, reg);
			if (mbi_phys_base == (phys_addr_t)OF_BAD_ADDR) {
				ret = -ENXIO;
				goto err_free_mbi;
			}
		} else {
			struct resource res;

			if (of_address_to_resource(np, 0, &res)) {
				ret = -ENXIO;
				goto err_free_mbi;
			}

			mbi_phys_base = res.start;
		}
		ret = mbi_allocate_domains(parent, parent->fwnode);
		if (ret)
			err_msg = "domain allocation failed";
		if (ret)
			goto err_free_mbi;
	} else if (mbi_acpi_cfg.nr_ranges) {
		mbi_range_nr = mbi_acpi_cfg.nr_ranges;
		if (!mbi_ranges) {
			mbi_ranges = kcalloc(mbi_range_nr, sizeof(*mbi_ranges),
					GFP_KERNEL);
			if (!mbi_ranges) {
				pr_err("MBI: kcalloc mbi_ranges failed (nr=%u size=%zu)\n",
						mbi_range_nr,
						array_size(mbi_range_nr, sizeof(*mbi_ranges)));
				return -ENOMEM;
			}
		}

		for (n = 0; n < mbi_range_nr; n++) {
			mbi_ranges[n].spi_start = mbi_acpi_cfg.ranges[n].spi_start;
			mbi_ranges[n].nr_spis = mbi_acpi_cfg.ranges[n].nr_spis;
			mbi_ranges[n].bm = bitmap_zalloc(mbi_ranges[n].nr_spis,
					GFP_KERNEL);
			if (!mbi_ranges[n].bm) {
				pr_err("MBI: bitmap_zalloc failed (nr_spis=%u)\n",
						mbi_ranges[n].nr_spis);
				ret = -ENOMEM;
				goto err_free_mbi;
			}
			pr_info("MBI range [%d:%d]\n", mbi_ranges[n].spi_start,
					mbi_ranges[n].spi_start + mbi_ranges[n].nr_spis - 1);
		}
		mbi_fwnode = irq_domain_alloc_named_fwnode("MBI");
		if (!mbi_fwnode) {
			pr_err("Unable to allocate GIC MBI domain token\n");
			return -EINVAL;
		}
		ret = mbi_allocate_domains(parent, mbi_fwnode);
		if (ret)
			err_msg = "domain allocation failed";
		if (ret)
			goto err_free_mbi;

		/* Spoof all 12 Phoenix ITSes in IORT */
		iort_register_domain_token(0, 0, mbi_fwnode);
		iort_register_domain_token(1, 0, mbi_fwnode);
		iort_register_domain_token(2, 0, mbi_fwnode);
		iort_register_domain_token(3, 0, mbi_fwnode);
		iort_register_domain_token(4, 0, mbi_fwnode);
		iort_register_domain_token(5, 0, mbi_fwnode);
		iort_register_domain_token(6, 0, mbi_fwnode);
		iort_register_domain_token(7, 0, mbi_fwnode);
		iort_register_domain_token(8, 0, mbi_fwnode);
		iort_register_domain_token(9, 0, mbi_fwnode);
		iort_register_domain_token(10, 0, mbi_fwnode);
		iort_register_domain_token(11, 0, mbi_fwnode);
	}

	return 0;

err_free_mbi:
	if (mbi_ranges) {
		for (n = 0; n < mbi_range_nr; n++)
			bitmap_free(mbi_ranges[n].bm);
		kfree(mbi_ranges);
	}
	if (mbi_fwnode) {
		irq_domain_free_fwnode(mbi_fwnode);
		mbi_fwnode = NULL;
	}

	return ret;
}
