// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 HiSilicon Limited, All Rights Reserved.
 * Author: Jun Ma <majun258@huawei.com>
 * Author: Yun Wu <wuyun.wu@huawei.com>
 */

#include <linux/acpi.h>
#include <linux/interrupt.h>
#include <linux/irqchip.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* Interrupt numbers per mbigen node supported */
#define IRQS_PER_MBIGEN_NODE		128

/* 64 irqs (Pin0-pin63) are reserved for each mbigen chip */
#define RESERVED_IRQ_PER_MBIGEN_CHIP	64

/* The maximum IRQ pin number of mbigen chip(start from 0) */
#define MAXIMUM_IRQ_PIN_NUM		1407

/*
 * In mbigen vector register
 * bit[21:12]:	event id value
 * bit[11:0]:	device id
 */
#define IRQ_EVENT_ID_SHIFT		12
#define IRQ_EVENT_ID_MASK		0x3ff

/* register range of each mbigen node */
#define MBIGEN_NODE_OFFSET		0x1000

/* offset of vector register in mbigen node */
#define REG_MBIGEN_VEC_OFFSET		0x200

/*
 * offset of clear register in mbigen node
 * This register is used to clear the status
 * of interrupt
 */
#define REG_MBIGEN_CLEAR_OFFSET		0xa000

/*
 * offset of interrupt type register
 * This register is used to configure interrupt
 * trigger type
 */
#define REG_MBIGEN_TYPE_OFFSET		0x0

#ifdef CONFIG_VIRT_VTIMER_IRQ_BYPASS
#include <linux/list.h>
#include <linux/cpumask.h>
#include <linux/cpuhotplug.h>
#include <asm/smp_plat.h>
#include <asm/cputype.h>
#include <asm/barrier.h>

#include <clocksource/arm_arch_timer.h>
#include <linux/irqchip/arm-gic-v3.h>

#define MBIGEN_CTLR_OFFSET                     0x0
#define MBIGEN_AFF3_MASK                       0xff000000
#define MBIGEN_AFF3_SHIFT                      24

/**
 * MBIX config register
 * bit[25:24] mbi_type:
 * - 0b10 support vtimer irqbypass
 */
#define MBIGEN_NODE_CFG_OFFSET                 0x0004
#define MBIGEN_TYPE_MASK                       0x03000000
#define MBIGEN_TYPE_SHIFT                      24
#define TYPE_VTIMER_ENABLED                    0x02

#define VTIMER_MBIGEN_REG_WIDTH                4
#define PPIS_PER_MBIGEN_NODE                   32
#define VTIMER_MBIGEN_REG_TYPE_OFFSET          0x1000
#define VTIMER_MBIGEN_REG_SET_AUTO_CLR_OFFSET  0x1100
#define VTIMER_MBIGEN_REG_CLR_AUTO_CLR_OFFSET  0x1110
#define VTIMER_MBIGEN_REG_ATV_STAT_OFFSET      0x1120
#define VTIMER_MBIGEN_REG_VEC_OFFSET           0x1200
#define VTIMER_MBIGEN_REG_ATV_CLR_OFFSET       0xa008

/**
 * struct vtimer_mbigen_device - holds the information of vtimer mbigen device.
 *
 * @base: mapped address of this mbigen chip.
 * @cpu_base : the base cpu_id attached to the mbigen chip.
 * @cpu_num : the num of the cpus attached to the mbigen chip.
 * @mpidr_aff3 : [socket_id : die_id] of the mbigen chip.
 * @entry: list_head connecting this vtimer_mbigen to the full list.
 * @vmgn_lock: spinlock for set type.
 */
struct vtimer_mbigen_device {
	void __iomem            *base;
	int                     cpu_base;
	int                     cpu_num;
	int                     mpidr_aff3;
	struct list_head        entry;
	spinlock_t              vmgn_lock;
};
#endif

/**
 * struct mbigen_device - holds the information of mbigen device.
 *
 * @pdev:		pointer to the platform device structure of mbigen chip.
 * @base:		mapped address of this mbigen chip.
 */
struct mbigen_device {
	struct platform_device	*pdev;
	void __iomem		*base;
#ifdef CONFIG_VIRT_VTIMER_IRQ_BYPASS
	struct vtimer_mbigen_device	*vtimer_mbigen_chip;
#endif
};

#ifdef CONFIG_VIRT_VTIMER_IRQ_BYPASS
static LIST_HEAD(vtimer_mgn_list);

/**
 * Due to the existence of hyper-threading technology, We need to get the
 * absolute offset of a cpu relative to the base cpu.
 */
#define GICR_LENGTH                            0x40000
static inline int get_abs_offset(int cpu, int cpu_base)
{
	return ((get_gicr_paddr(cpu) - get_gicr_paddr(cpu_base)) / GICR_LENGTH);
}

static struct vtimer_mbigen_device *get_vtimer_mbigen(int cpu_id)
{
	unsigned int mpidr_aff3;
	struct vtimer_mbigen_device *chip;

	mpidr_aff3 = MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu_id), 3);

	list_for_each_entry(chip, &vtimer_mgn_list, entry) {
		if (chip->mpidr_aff3 == mpidr_aff3)
			return chip;
	}

	pr_debug("Failed to get vtimer mbigen of cpu%d!\n", cpu_id);
	return NULL;
}

void vtimer_mbigen_set_vector(int cpu_id, u16 vpeid)
{

	struct vtimer_mbigen_device *chip;
	void __iomem *addr;
	int cpu_abs_offset, count = 100;

	chip = get_vtimer_mbigen(cpu_id);
	if (!chip)
		return;

	cpu_abs_offset = get_abs_offset(cpu_id, chip->cpu_base);
	addr = chip->base + VTIMER_MBIGEN_REG_VEC_OFFSET +
	       cpu_abs_offset * VTIMER_MBIGEN_REG_WIDTH;

	writel_relaxed(vpeid, addr);

	/* Make sure correct vpeid set */
	do {
		if (readl_relaxed(addr) == vpeid)
			break;
	} while (count--);

	if (!count)
		pr_err("Failed to set mbigen vector of CPU%d!\n", cpu_id);
}

bool vtimer_mbigen_get_active(int cpu_id)
{
	struct vtimer_mbigen_device *chip;
	void __iomem *addr;
	int cpu_abs_offset;
	u32 val;

	chip = get_vtimer_mbigen(cpu_id);
	if (!chip)
		return false;

	cpu_abs_offset = get_abs_offset(cpu_id, chip->cpu_base);
	addr = chip->base + VTIMER_MBIGEN_REG_ATV_STAT_OFFSET +
		(cpu_abs_offset / PPIS_PER_MBIGEN_NODE) * VTIMER_MBIGEN_REG_WIDTH;

	dsb(sy);
	val = readl_relaxed(addr);
	return (!!(val & (1 << (cpu_abs_offset % PPIS_PER_MBIGEN_NODE))));
}

void vtimer_mbigen_set_auto_clr(int cpu_id, bool set)
{
	struct vtimer_mbigen_device *chip;
	void __iomem *addr;
	int cpu_abs_offset;
	u64 offset;
	u32 val;

	chip = get_vtimer_mbigen(cpu_id);
	if (!chip)
		return;

	cpu_abs_offset = get_abs_offset(cpu_id, chip->cpu_base);
	offset = set ? VTIMER_MBIGEN_REG_SET_AUTO_CLR_OFFSET :
		 VTIMER_MBIGEN_REG_CLR_AUTO_CLR_OFFSET;
	addr = chip->base + offset +
		(cpu_abs_offset / PPIS_PER_MBIGEN_NODE) * VTIMER_MBIGEN_REG_WIDTH;
	val = 1 << (cpu_abs_offset % PPIS_PER_MBIGEN_NODE);

	writel_relaxed(val, addr);
	dsb(sy);
}

void vtimer_mbigen_set_active(int cpu_id, bool set)
{
	struct vtimer_mbigen_device *chip;
	void __iomem *addr;
	int cpu_abs_offset;
	u64 offset;
	u32 val;

	chip = get_vtimer_mbigen(cpu_id);
	if (!chip)
		return;

	cpu_abs_offset = get_abs_offset(cpu_id, chip->cpu_base);
	offset = set ? VTIMER_MBIGEN_REG_ATV_STAT_OFFSET :
		 VTIMER_MBIGEN_REG_ATV_CLR_OFFSET;
	addr = chip->base + offset +
		(cpu_abs_offset / PPIS_PER_MBIGEN_NODE) * VTIMER_MBIGEN_REG_WIDTH;
	val = 1 << (cpu_abs_offset % PPIS_PER_MBIGEN_NODE);

	writel_relaxed(val, addr);
	dsb(sy);
}

static int vtimer_mbigen_set_type(unsigned int cpu_id)
{
	struct vtimer_mbigen_device *chip;
	void __iomem *addr;
	int cpu_abs_offset;
	u32 val, mask;

	chip = get_vtimer_mbigen(cpu_id);
	if (!chip)
		return -EINVAL;

	cpu_abs_offset = get_abs_offset(cpu_id, chip->cpu_base);
	addr = chip->base + VTIMER_MBIGEN_REG_TYPE_OFFSET +
	       (cpu_abs_offset / PPIS_PER_MBIGEN_NODE) * VTIMER_MBIGEN_REG_WIDTH;

	mask = 1 << (cpu_abs_offset % PPIS_PER_MBIGEN_NODE);

	spin_lock(&chip->vmgn_lock);
	val = readl_relaxed(addr);
	val |= mask;
	writel_relaxed(val, addr);
	dsb(sy);
	spin_unlock(&chip->vmgn_lock);
	return 0;
}
#endif

static inline unsigned int get_mbigen_node_offset(unsigned int nid)
{
	unsigned int offset = nid * MBIGEN_NODE_OFFSET;

	/*
	 * To avoid touched clear register in unexpected way, we need to directly
	 * skip clear register when access to more than 10 mbigen nodes.
	 */
	if (nid >= (REG_MBIGEN_CLEAR_OFFSET / MBIGEN_NODE_OFFSET))
		offset += MBIGEN_NODE_OFFSET;

	return offset;
}

static inline unsigned int get_mbigen_vec_reg(irq_hw_number_t hwirq)
{
	unsigned int nid, pin;

	hwirq -= RESERVED_IRQ_PER_MBIGEN_CHIP;
	nid = hwirq / IRQS_PER_MBIGEN_NODE + 1;
	pin = hwirq % IRQS_PER_MBIGEN_NODE;

	return pin * 4 + get_mbigen_node_offset(nid) + REG_MBIGEN_VEC_OFFSET;
}

static inline void get_mbigen_type_reg(irq_hw_number_t hwirq,
					u32 *mask, u32 *addr)
{
	unsigned int nid, irq_ofst, ofst;

	hwirq -= RESERVED_IRQ_PER_MBIGEN_CHIP;
	nid = hwirq / IRQS_PER_MBIGEN_NODE + 1;
	irq_ofst = hwirq % IRQS_PER_MBIGEN_NODE;

	*mask = 1 << (irq_ofst % 32);
	ofst = irq_ofst / 32 * 4;

	*addr = ofst + get_mbigen_node_offset(nid) + REG_MBIGEN_TYPE_OFFSET;
}

static inline void get_mbigen_clear_reg(irq_hw_number_t hwirq,
					u32 *mask, u32 *addr)
{
	unsigned int ofst = (hwirq / 32) * 4;

	*mask = 1 << (hwirq % 32);
	*addr = ofst + REG_MBIGEN_CLEAR_OFFSET;
}

static void mbigen_eoi_irq(struct irq_data *data)
{
	void __iomem *base = data->chip_data;
	u32 mask, addr;

	get_mbigen_clear_reg(data->hwirq, &mask, &addr);

	writel_relaxed(mask, base + addr);

	irq_chip_eoi_parent(data);
}

static int mbigen_set_type(struct irq_data *data, unsigned int type)
{
	void __iomem *base = data->chip_data;
	u32 mask, addr, val;

	if (type != IRQ_TYPE_LEVEL_HIGH && type != IRQ_TYPE_EDGE_RISING)
		return -EINVAL;

	get_mbigen_type_reg(data->hwirq, &mask, &addr);

	val = readl_relaxed(base + addr);

	if (type == IRQ_TYPE_LEVEL_HIGH)
		val |= mask;
	else
		val &= ~mask;

	writel_relaxed(val, base + addr);

	return 0;
}

static struct irq_chip mbigen_irq_chip = {
	.name =			"mbigen-v2",
	.irq_mask =		irq_chip_mask_parent,
	.irq_unmask =		irq_chip_unmask_parent,
	.irq_eoi =		mbigen_eoi_irq,
	.irq_set_type =		mbigen_set_type,
	.irq_set_affinity =	irq_chip_set_affinity_parent,
};

static void mbigen_write_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	struct irq_data *d = irq_get_irq_data(desc->irq);
	void __iomem *base = d->chip_data;
	u32 val;

	if (!msg->address_lo && !msg->address_hi)
		return;
 
	base += get_mbigen_vec_reg(d->hwirq);
	val = readl_relaxed(base);

	val &= ~(IRQ_EVENT_ID_MASK << IRQ_EVENT_ID_SHIFT);
	val |= (msg->data << IRQ_EVENT_ID_SHIFT);

	/* The address of doorbell is encoded in mbigen register by default
	 * So,we don't need to program the doorbell address at here
	 */
	writel_relaxed(val, base);
}

static int mbigen_domain_translate(struct irq_domain *d,
				    struct irq_fwspec *fwspec,
				    unsigned long *hwirq,
				    unsigned int *type)
{
	if (is_of_node(fwspec->fwnode) || is_acpi_device_node(fwspec->fwnode)) {
		if (fwspec->param_count != 2)
			return -EINVAL;

		if ((fwspec->param[0] > MAXIMUM_IRQ_PIN_NUM) ||
			(fwspec->param[0] < RESERVED_IRQ_PER_MBIGEN_CHIP))
			return -EINVAL;
		else
			*hwirq = fwspec->param[0];

		/* If there is no valid irq type, just use the default type */
		if ((fwspec->param[1] == IRQ_TYPE_EDGE_RISING) ||
			(fwspec->param[1] == IRQ_TYPE_LEVEL_HIGH))
			*type = fwspec->param[1];
		else
			return -EINVAL;

		return 0;
	}
	return -EINVAL;
}

static int mbigen_irq_domain_alloc(struct irq_domain *domain,
					unsigned int virq,
					unsigned int nr_irqs,
					void *args)
{
	struct irq_fwspec *fwspec = args;
	irq_hw_number_t hwirq;
	unsigned int type;
	struct mbigen_device *mgn_chip;
	int i, err;

	err = mbigen_domain_translate(domain, fwspec, &hwirq, &type);
	if (err)
		return err;

	err = platform_msi_device_domain_alloc(domain, virq, nr_irqs);
	if (err)
		return err;

	mgn_chip = platform_msi_get_host_data(domain);

	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
				      &mbigen_irq_chip, mgn_chip->base);

	return 0;
}

static void mbigen_irq_domain_free(struct irq_domain *domain, unsigned int virq,
				   unsigned int nr_irqs)
{
	platform_msi_device_domain_free(domain, virq, nr_irqs);
}

static const struct irq_domain_ops mbigen_domain_ops = {
	.translate	= mbigen_domain_translate,
	.alloc		= mbigen_irq_domain_alloc,
	.free		= mbigen_irq_domain_free,
};

static int mbigen_of_create_domain(struct platform_device *pdev,
				   struct mbigen_device *mgn_chip)
{
	struct platform_device *child;
	struct irq_domain *domain;
	struct device_node *np;
	u32 num_pins;
	int ret = 0;

	for_each_child_of_node(pdev->dev.of_node, np) {
		if (!of_property_read_bool(np, "interrupt-controller"))
			continue;

		child = of_platform_device_create(np, NULL, NULL);
		if (!child) {
			ret = -ENOMEM;
			break;
		}

		if (of_property_read_u32(child->dev.of_node, "num-pins",
					 &num_pins) < 0) {
			dev_err(&pdev->dev, "No num-pins property\n");
			ret = -EINVAL;
			break;
		}

		domain = platform_msi_create_device_domain(&child->dev, num_pins,
							   mbigen_write_msg,
							   &mbigen_domain_ops,
							   mgn_chip);
		if (!domain) {
			ret = -ENOMEM;
			break;
		}
	}

	if (ret)
		of_node_put(np);

	return ret;
}

#ifdef CONFIG_ACPI
static const struct acpi_device_id mbigen_acpi_match[] = {
	{ "HISI0152", 0 },
	{}
};
MODULE_DEVICE_TABLE(acpi, mbigen_acpi_match);

static int mbigen_acpi_create_domain(struct platform_device *pdev,
				     struct mbigen_device *mgn_chip)
{
	struct irq_domain *domain;
	u32 num_pins = 0;
	int ret;

	/*
	 * "num-pins" is the total number of interrupt pins implemented in
	 * this mbigen instance, and mbigen is an interrupt controller
	 * connected to ITS  converting wired interrupts into MSI, so we
	 * use "num-pins" to alloc MSI vectors which are needed by client
	 * devices connected to it.
	 *
	 * Here is the DSDT device node used for mbigen in firmware:
	 *	Device(MBI0) {
	 *		Name(_HID, "HISI0152")
	 *		Name(_UID, Zero)
	 *		Name(_CRS, ResourceTemplate() {
	 *			Memory32Fixed(ReadWrite, 0xa0080000, 0x10000)
	 *		})
	 *
	 *		Name(_DSD, Package () {
	 *			ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
	 *			Package () {
	 *				Package () {"num-pins", 378}
	 *			}
	 *		})
	 *	}
	 */
	ret = device_property_read_u32(&pdev->dev, "num-pins", &num_pins);
	if (ret || num_pins == 0)
		return -EINVAL;

	domain = platform_msi_create_device_domain(&pdev->dev, num_pins,
						   mbigen_write_msg,
						   &mbigen_domain_ops,
						   mgn_chip);
	if (!domain)
		return -ENOMEM;

	return 0;
}
#else
static inline int mbigen_acpi_create_domain(struct platform_device *pdev,
					    struct mbigen_device *mgn_chip)
{
	return -ENODEV;
}
#endif

#ifdef CONFIG_VIRT_VTIMER_IRQ_BYPASS
static void vtimer_mbigen_set_kvm_info(void)
{
	struct arch_timer_kvm_info *info = arch_timer_get_kvm_info();

	info->irqbypass_flag |= VT_EXPANDDEV_PROBED;
}

static int vtimer_mbigen_chip_read_aff3(struct vtimer_mbigen_device *chip)
{
	void __iomem *base = chip->base;
	void __iomem *addr = base + MBIGEN_CTLR_OFFSET;
	u32 val = readl_relaxed(addr);

	return ((val & MBIGEN_AFF3_MASK) >> MBIGEN_AFF3_SHIFT);
}

static int vtimer_mbigen_chip_match_cpu(struct vtimer_mbigen_device *chip)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		int mpidr_aff3 = MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 3);

		if (chip->mpidr_aff3 == mpidr_aff3) {
			/* get the first cpu attached to the mbigen */
			if (chip->cpu_base == -1) {
				/* Make sure cpu_base is attached to PIN0 */
				u64 mpidr = cpu_logical_map(cpu);

				if (!MPIDR_AFFINITY_LEVEL(mpidr, 2) &&
				    !MPIDR_AFFINITY_LEVEL(mpidr, 1) &&
				    !MPIDR_AFFINITY_LEVEL(mpidr, 0))
					chip->cpu_base = cpu;
			}
		}
	}

	if (chip->cpu_base == -1)
		return -EINVAL;

	return 0;
}

static bool is_mbigen_vtimer_bypass_enabled(struct mbigen_device *mgn_chip)
{
	void __iomem *base = mgn_chip->base;
	void __iomem *addr = base + MBIGEN_NODE_CFG_OFFSET;
	u32 val = readl_relaxed(addr);

	return ((val & MBIGEN_TYPE_MASK) >> MBIGEN_TYPE_SHIFT)
		== TYPE_VTIMER_ENABLED;
}

/**
 * MBIX_VPPI_ITS_TA: Indicates the address of the ITS corresponding
 * to the mbigen.
 */
#define MBIX_VPPI_ITS_TA	0x0038
static bool vtimer_mbigen_should_probe(struct mbigen_device *mgn_chip)
{
	unsigned int mpidr_aff3;
	struct vtimer_mbigen_device *chip;
	void __iomem *addr;
	u32 val;

	/* find the valid mbigen */
	addr = mgn_chip->base + MBIX_VPPI_ITS_TA;
	val = readl_relaxed(addr);
	if (!val)
		return false;

	addr = mgn_chip->base + MBIGEN_CTLR_OFFSET;
	val = readl_relaxed(addr);
	mpidr_aff3 = (val & MBIGEN_AFF3_MASK) >> MBIGEN_AFF3_SHIFT;
	list_for_each_entry(chip, &vtimer_mgn_list, entry) {
		if (chip->mpidr_aff3 == mpidr_aff3)
			return false;
	}

	return true;
}

static int vtimer_mbigen_device_probe(struct platform_device *pdev)
{
	struct mbigen_device *mgn_chip = platform_get_drvdata(pdev);
	struct vtimer_mbigen_device *vtimer_mgn_chip;
	int err;

	if (!is_mbigen_vtimer_bypass_enabled(mgn_chip) ||
	    !vtimer_mbigen_should_probe(mgn_chip))
		return 0;

	vtimer_mgn_chip = kzalloc(sizeof(*vtimer_mgn_chip), GFP_KERNEL);
	if (!vtimer_mgn_chip)
		return -ENOMEM;

	mgn_chip->vtimer_mbigen_chip = vtimer_mgn_chip;
	vtimer_mgn_chip->base = mgn_chip->base;
	vtimer_mgn_chip->mpidr_aff3 = vtimer_mbigen_chip_read_aff3(vtimer_mgn_chip);
	vtimer_mgn_chip->cpu_base = -1;
	err = vtimer_mbigen_chip_match_cpu(vtimer_mgn_chip);
	if (err) {
		dev_err(&pdev->dev,
			"Fail to match vtimer mbigen device with cpu\n");
		goto out;
	}

	spin_lock_init(&vtimer_mgn_chip->vmgn_lock);
	list_add(&vtimer_mgn_chip->entry, &vtimer_mgn_list);
	vtimer_mbigen_set_kvm_info();
	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "irqchip/mbigen-vtimer:online",
			  vtimer_mbigen_set_type, NULL);

	pr_info("vtimer mbigen device @%p probed success!\n", mgn_chip->base);
	return 0;

out:
	kfree(vtimer_mgn_chip);
	dev_err(&pdev->dev, "vtimer mbigen device @%p probed failed\n",
		mgn_chip->base);
	return err;
}
#endif

static int mbigen_device_probe(struct platform_device *pdev)
{
	struct mbigen_device *mgn_chip;
	struct resource *res;
	int err;

	mgn_chip = devm_kzalloc(&pdev->dev, sizeof(*mgn_chip), GFP_KERNEL);
	if (!mgn_chip)
		return -ENOMEM;

	mgn_chip->pdev = pdev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	mgn_chip->base = devm_ioremap(&pdev->dev, res->start,
				      resource_size(res));
	if (!mgn_chip->base) {
		dev_err(&pdev->dev, "failed to ioremap %pR\n", res);
		return -ENOMEM;
	}

	if (IS_ENABLED(CONFIG_OF) && pdev->dev.of_node)
		err = mbigen_of_create_domain(pdev, mgn_chip);
	else if (ACPI_COMPANION(&pdev->dev))
		err = mbigen_acpi_create_domain(pdev, mgn_chip);
	else
		err = -EINVAL;

	if (err) {
		dev_err(&pdev->dev, "Failed to create mbi-gen irqdomain\n");
		return err;
	}

	platform_set_drvdata(pdev, mgn_chip);

#ifdef CONFIG_VIRT_VTIMER_IRQ_BYPASS
	err = vtimer_mbigen_device_probe(pdev);

	if (err) {
		dev_err(&pdev->dev, "Failed to probe vtimer mbigen device\n");
		return err;
	}
#endif
	return 0;
}

static const struct of_device_id mbigen_of_match[] = {
	{ .compatible = "hisilicon,mbigen-v2" },
	{ /* END */ }
};
MODULE_DEVICE_TABLE(of, mbigen_of_match);

static struct platform_driver mbigen_platform_driver = {
	.driver = {
		.name		= "Hisilicon MBIGEN-V2",
		.of_match_table	= mbigen_of_match,
		.acpi_match_table = ACPI_PTR(mbigen_acpi_match),
		.suppress_bind_attrs = true,
	},
	.probe			= mbigen_device_probe,
};

module_platform_driver(mbigen_platform_driver);

MODULE_AUTHOR("Jun Ma <majun258@huawei.com>");
MODULE_AUTHOR("Yun Wu <wuyun.wu@huawei.com>");
MODULE_DESCRIPTION("HiSilicon MBI Generator driver");
