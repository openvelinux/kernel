// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Synopsys DesignWare Core I2C adapter driver.
 *
 * Copyright (C) 2024 Socionext Inc.
 * Portions Copyright (c) 2024 Synopsys, Inc. Used with permission. All rights reserved.
 *
 * This is a derivative work of Synopsys DesignWare I2C adapter driver.
 * - drivers/i2c/busses/i2c-designware-platdrv.c
 *
 * Copyright (C) 2006 Texas Instruments.
 * Copyright (C) 2007 MontaVista Software Inc.
 * Copyright (C) 2009 Provigent Ltd.
 */
#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/suspend.h>
#include <linux/units.h>

#include "core.h"

static u32 dwc_i2c_get_clk_rate_khz(struct dwc_i2c_dev *dev)
{
	return clk_get_rate(dev->clk) / KILO;
}

#ifdef CONFIG_ACPI
static const struct acpi_device_id dwc_i2c_acpi_match[] = {
	{ "PHNX0001", 0 }, /* TODO: Update with valid ACPI ID */
	{},
};
MODULE_DEVICE_TABLE(acpi, dwc_i2c_acpi_match);
#endif

#ifdef CONFIG_OF
static const struct of_device_id dwc_i2c_of_match[] = {
	{
		.compatible = "snps,dwc-i2c",
	},
	{},
};
MODULE_DEVICE_TABLE(of, dwc_i2c_of_match);
#endif

static void dwc_i2c_plat_pm_cleanup(struct dwc_i2c_dev *dev)
{
	pm_runtime_disable(dev->dev);
}

static int dwc_i2c_plat_request_regs(struct dwc_i2c_dev *dev)
{
	struct platform_device *pdev = to_platform_device(dev->dev);
	struct resource *res;

	dev->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(dev->base))
		return PTR_ERR(dev->base);

	dev->reg_size = resource_size(res);

	return 0;
}

static const struct dmi_system_id dwc_i2c_hwmon_class_dmi[] = {
	{}, /* terminate list */
};

static int dwc_i2c_plat_probe(struct platform_device *pdev)
{
	struct i2c_adapter *adap;
	struct dwc_i2c_dev *dev;
	int irq, ret;
	u32 dpm_flags;
	const struct platform_device_id *pdev_id = platform_get_device_id(pdev);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->flags = pdev_id ? pdev_id->driver_data :
			       (uintptr_t)device_get_match_data(&pdev->dev);

	dev->dev = &pdev->dev;
	dev->irq = irq;
	platform_set_drvdata(pdev, dev);

	ret = dwc_i2c_plat_request_regs(dev);
	if (ret)
		return ret;

	dev->rst = devm_reset_control_get_optional_exclusive(&pdev->dev, NULL);
	if (IS_ERR(dev->rst))
		return PTR_ERR(dev->rst);

	reset_control_deassert(dev->rst);

	/* Optional interface clock */
	dev->pclk = devm_clk_get_optional(&pdev->dev, "pclk");
	if (IS_ERR(dev->pclk)) {
		ret = PTR_ERR(dev->pclk);
		goto exit_reset;
	}

	dev->clk = devm_clk_get_optional(&pdev->dev, NULL);
	if (IS_ERR(dev->clk)) {
		ret = PTR_ERR(dev->clk);
		goto exit_reset;
	}

	ret = dwc_i2c_prepare_clk(dev, true);
	if (ret)
		goto exit_reset;

	if (dev->clk)
		dev->get_clk_rate_khz = dwc_i2c_get_clk_rate_khz;

	adap = &dev->adapter;
	adap->owner = THIS_MODULE;
	adap->class = dmi_check_system(dwc_i2c_hwmon_class_dmi) ?
			      I2C_CLASS_HWMON :
			      I2C_CLASS_DEPRECATED;
	adap->nr = -1;

	if (dev->flags & ACCESS_NO_IRQ_SUSPEND)
		dpm_flags = DPM_FLAG_SMART_PREPARE;
	else
		dpm_flags = DPM_FLAG_SMART_PREPARE | DPM_FLAG_SMART_SUSPEND;
	dev_pm_set_driver_flags(&pdev->dev, dpm_flags);

	device_enable_async_suspend(&pdev->dev);

	/* The code below assumes runtime PM to be disabled. */
	WARN_ON(pm_runtime_enabled(&pdev->dev));

	pm_runtime_set_autosuspend_delay(&pdev->dev, 1000);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);

	pm_runtime_enable(&pdev->dev);

	ret = dwc_i2c_probe(dev);
	if (ret)
		goto exit_probe;

	return ret;

exit_probe:
	dwc_i2c_plat_pm_cleanup(dev);
	dwc_i2c_prepare_clk(dev, false);
exit_reset:
	reset_control_assert(dev->rst);
	return ret;
}

static void dwc_i2c_plat_remove(struct platform_device *pdev)
{
	struct dwc_i2c_dev *dev = platform_get_drvdata(pdev);

	pm_runtime_get_sync(&pdev->dev);

	i2c_del_adapter(&dev->adapter);

	dwc_i2c_disable(dev);

	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
	dwc_i2c_plat_pm_cleanup(dev);

	dwc_i2c_prepare_clk(dev, false);

	reset_control_assert(dev->rst);
}

static int dwc_i2c_plat_prepare(struct device *dev)
{
	/*
	 * If the ACPI companion device object is present for this device, it
	 * may be accessed during suspend and resume of other devices via I2C
	 * operation regions, so tell the PM core and middle layers to avoid
	 * skipping system suspend/resume callbacks for it in that case.
	 */
	return !has_acpi_companion(dev);
}

static int dwc_i2c_plat_runtime_suspend(struct device *dev)
{
	struct dwc_i2c_dev *i_dev = dev_get_drvdata(dev);

	dwc_i2c_disable(i_dev);
	dwc_i2c_prepare_clk(i_dev, false);

	return 0;
}

static int dwc_i2c_plat_suspend(struct device *dev)
{
	struct dwc_i2c_dev *i_dev = dev_get_drvdata(dev);

	i2c_mark_adapter_suspended(&i_dev->adapter);

	return dwc_i2c_plat_runtime_suspend(dev);
}

static int dwc_i2c_plat_runtime_resume(struct device *dev)
{
	struct dwc_i2c_dev *i_dev = dev_get_drvdata(dev);

	dwc_i2c_prepare_clk(i_dev, true);

	i_dev->init(i_dev);

	return 0;
}

static int dwc_i2c_plat_resume(struct device *dev)
{
	struct dwc_i2c_dev *i_dev = dev_get_drvdata(dev);

	dwc_i2c_plat_runtime_resume(dev);
	i2c_mark_adapter_resumed(&i_dev->adapter);

	return 0;
}

static const struct dev_pm_ops dwc_i2c_dev_pm_ops = {
	.prepare = pm_sleep_ptr(dwc_i2c_plat_prepare),
	LATE_SYSTEM_SLEEP_PM_OPS(dwc_i2c_plat_suspend, dwc_i2c_plat_resume)
		RUNTIME_PM_OPS(dwc_i2c_plat_runtime_suspend,
			       dwc_i2c_plat_runtime_resume, NULL)
};

static const struct platform_device_id dw_i2c_platform_ids[] = {
	{
		.name = "i2c-dwc",
	},
	{}
};
MODULE_DEVICE_TABLE(platform, dw_i2c_platform_ids);

static struct platform_driver dwc_i2c_driver = {
	.probe = dwc_i2c_plat_probe,
	.remove_new = dwc_i2c_plat_remove,
	.driver		= {
		.name	= "i2c-dwc",
		.of_match_table = of_match_ptr(dwc_i2c_of_match),
		.acpi_match_table = ACPI_PTR(dwc_i2c_acpi_match),
		.pm	= pm_ptr(&dwc_i2c_dev_pm_ops),
	},
	.id_table = dw_i2c_platform_ids,
};

static int __init dwc_i2c_init_driver(void)
{
	return platform_driver_register(&dwc_i2c_driver);
}
subsys_initcall(dwc_i2c_init_driver);

static void __exit dwc_i2c_exit_driver(void)
{
	platform_driver_unregister(&dwc_i2c_driver);
}
module_exit(dwc_i2c_exit_driver);

MODULE_AUTHOR("Socionext Inc.");
MODULE_DESCRIPTION("Synopsys DesignWare Core I2C bus adapter");
MODULE_LICENSE("GPL");
