// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019-2020 HUAWEI TECHNOLOGIES CO., LTD., All Rights Reserved.
 * Author: Wanghaibin <wanghaibin.wang@huawei.com>
 */
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/device.h>

#define VIRT_DEV_DEBUG 1

#ifdef VIRT_DEV_DEBUG
#define virtdev_info(fmt, ...)	pr_info("virdev: " fmt, ## __VA_ARGS__)
#else
#define virtdev_info(fmt, ...)
#endif

static irqreturn_t virt_irq_handle(int irq, void *data)
{
	return IRQ_HANDLED;
}

static void virt_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
}

static int virt_device_probe(struct platform_device *pdev)
{
	struct msi_desc *desc;
	unsigned int *drvdata = dev_get_drvdata(&pdev->dev);
	unsigned int nvec = *drvdata;
	struct irq_domain *vp_irqdomain = vp_get_irq_domain();
	int ret;

	if (!vp_irqdomain)
		return -ENXIO;

	virtdev_info("Allocate platform msi irqs nvecs: %d\n", nvec);
	dev_set_msi_domain(&pdev->dev, vp_irqdomain);

	ret = platform_msi_domain_alloc_irqs(&pdev->dev, nvec,
					     virt_write_msi_msg);
	if (ret) {
		pr_err("Allocate platform msi irqs failed %d\n", ret);
		goto error;
	}

	virtdev_info("Allocate platform msi irqs succeed\n");
	msi_for_each_desc(desc, &pdev->dev, MSI_DESC_ALL) {
		virtdev_info("Request irq %d\n", desc->irq);
		ret = request_irq(desc->irq, virt_irq_handle, 0,
				  "virt_dev_host", pdev);
		if (ret) {
			pr_err("Request irq %d failed %d\n", desc->irq, ret);
			goto error_free_irqs;
		}
	}

	virtdev_info("Init virtual platform device driver successfully.\n");
	return 0;

error_free_irqs:
	msi_for_each_desc(desc, &pdev->dev, MSI_DESC_ALL)
		free_irq(desc->irq, pdev);

	platform_msi_domain_free_irqs(&pdev->dev);
error:
	return ret;
}

static int virt_device_remove(struct platform_device *pdev)
{
	struct msi_desc *desc;

	msi_for_each_desc(desc, &pdev->dev, MSI_DESC_ALL)
		free_irq(desc->irq, pdev);

	platform_msi_domain_free_irqs(&pdev->dev);

	return 0;
}

static struct platform_driver virtdev_driver = {
	.driver = {
		/* Using the device & driver name to match each other */
		.name = "virt_plat_dev",
	},
	.probe = virt_device_probe,
	.remove = virt_device_remove,
};

static int __init virtdev_init(void)
{
	int ret;

	ret = platform_driver_register(&virtdev_driver);
	if (ret) {
		pr_err("Register virtdev platform driver failed (%d)\n", ret);
		return ret;
	}

	virtdev_info("Register virtdev platform driver succeed.\n");
	return 0;
}
module_init(virtdev_init);

static void  __exit virtdev_exit(void)
{
	platform_driver_unregister(&virtdev_driver);
}
module_exit(virtdev_exit);

MODULE_LICENSE("GPL");
