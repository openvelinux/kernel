// SPDX-License-Identifier: GPL-2.0-only
/*
 * This file implements handling of
 * Arm Generic Diagnostic Dump and Reset Interface table (AGDI)
 *
 * Copyright (c) 2022, Ampere Computing LLC
 */

#define pr_fmt(fmt) "ACPI: AGDI: " fmt

#include <linux/acpi.h>
#include <linux/arm_sdei.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <asm/irq_regs.h>
#include <linux/nmi.h>
#include <linux/sched/debug.h>
#include <linux/irq_work.h>
#include <linux/workqueue.h>
#include <linux/sysctl.h>
#include <linux/platform_device.h>
#include "init.h"

static int panic_on_acpi_nmi = 1;
static int showmem_on_acpi_nmi = 1;
static int showcpus_on_acpi_nmi = 1;

static struct irq_work dump_irq_work;
/* Fallback when NMI is not supported */
static struct work_struct show_cpus_work;

#ifdef CONFIG_SYSCTL
#include <linux/sysctl.h>
static struct ctl_table_header *hdr;

static struct ctl_table acpi_nmi_table[] = {
	{
		.procname	= "panic_enable",
		.data		= &panic_on_acpi_nmi,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "show_mem_usage_enable",
		.data		= &showmem_on_acpi_nmi,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "show_allcpus_backtrace_enable",
		.data		= &showcpus_on_acpi_nmi,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{ }
};
static int register_nmi_sysctls(void)
{
	if (!hdr) {
		hdr = register_sysctl("fs/acpi_nmi", acpi_nmi_table);
		if (!hdr)
			return -ENOMEM;
	}
	return 0;
}

static void unregister_nmi_sysctls(void)
{
	if (hdr)
		unregister_sysctl_table(hdr);
	hdr = NULL;
}

#else
static int register_nmi_sysctls(void) { return 0; }
static void unregister_nmi_sysctls(void) {}
#endif

struct agdi_data {
	int sdei_event;
};

static void show_all_cpus(void)
{
	/*
	 * Fall back to the workqueue based printing if the
	 * backtrace printing did not succeed or the
	 * architecture has no support for it:
	 */
	if (!trigger_all_cpu_backtrace()) {
		struct pt_regs *regs = NULL;
		int cpu;

		preempt_disable();
		cpu = smp_processor_id();
		preempt_enable();

		if (idle_cpu(cpu)) {
			pr_emerg("CPU%d: backtrace skipped as idling\n", cpu);
			return;
		}

		if (in_irq())
			regs = get_irq_regs();

		pr_emerg("CPU%d:\n", cpu);
		if (regs)
			show_regs(regs);
		else
			show_stack(NULL, NULL, KERN_DEFAULT);

		queue_work(system_highpri_wq, &show_cpus_work);
	}
}

static void show_memory(void)
{
	show_mem();
}

static void dump_in_irq(struct irq_work *irq_work)
{
	int old_lvl;

	old_lvl = console_loglevel;
	console_loglevel = CONSOLE_LOGLEVEL_MOTORMOUTH;
	pr_emerg("Start dumping, triggered by BMC");
	if (showmem_on_acpi_nmi)
		show_memory();

	if (showcpus_on_acpi_nmi)
		show_all_cpus();

	if (panic_on_acpi_nmi)
		nmi_panic(NULL, "Panic triggered by BMC!!\n");

	console_loglevel = old_lvl;
}

static DEFINE_RAW_SPINLOCK(show_lock);

static void showacpu(void *dummy)
{
	unsigned long flags;
	int old_lvl;

	old_lvl = console_loglevel;
	/* Idle CPUs have no interesting backtrace. */
	if (idle_cpu(smp_processor_id())) {
		console_loglevel = CONSOLE_LOGLEVEL_MOTORMOUTH;
		pr_emerg("CPU%d: backtrace skipped as idling\n", smp_processor_id());
		console_loglevel = old_lvl;
		return;
	}

	raw_spin_lock_irqsave(&show_lock, flags);
	console_loglevel = CONSOLE_LOGLEVEL_MOTORMOUTH;
	pr_emerg("CPU%d:\n", smp_processor_id());
	show_stack(NULL, NULL, KERN_DEFAULT);
	console_loglevel = old_lvl;
	raw_spin_unlock_irqrestore(&show_lock, flags);
}

static void showcpus_fb(struct work_struct *work)
{
	smp_call_function(showacpu, NULL, 0);
}

static void init_work_queue(void)
{
	init_irq_work(&dump_irq_work, dump_in_irq);
	INIT_WORK(&show_cpus_work, showcpus_fb);
}

static int agdi_sdei_handler(u32 sdei_event, struct pt_regs *regs, void *arg)
{
	pr_debug("SDEI callback\n");
	irq_work_queue(&dump_irq_work);
	return 0;
}

static int agdi_sdei_probe(struct platform_device *pdev,
			   struct agdi_data *adata)
{
	int err;

	err = sdei_event_register(adata->sdei_event, agdi_sdei_handler, pdev);
	if (err) {
		dev_err(&pdev->dev, "Failed to register for SDEI event %d",
			adata->sdei_event);
		return err;
	}

	init_work_queue();

	err = sdei_event_enable(adata->sdei_event);
	if (err)  {
		sdei_event_unregister(adata->sdei_event);
		dev_err(&pdev->dev, "Failed to enable event %d\n",
			adata->sdei_event);
		return err;
	}

    /* Register sysctl interface */
	err = register_nmi_sysctls();
	if (err) {
		dev_err(&pdev->dev, "Failed to register sysctl interface\n");
		sdei_event_unregister(adata->sdei_event);
		return err;
	}

	pr_debug("sdei-event #%d registered\n", adata->sdei_event);

	return 0;
}

static int agdi_probe(struct platform_device *pdev)
{
	struct agdi_data *adata = dev_get_platdata(&pdev->dev);

	if (!adata)
		return -EINVAL;

	return agdi_sdei_probe(pdev, adata);
}

static int agdi_remove(struct platform_device *pdev)
{
	struct agdi_data *adata = dev_get_platdata(&pdev->dev);
	int err, i;

	unregister_nmi_sysctls();

	err = sdei_event_disable(adata->sdei_event);
	if (err) {
		dev_err(&pdev->dev, "Failed to disable sdei-event #%d (%pe)\n",
			adata->sdei_event, ERR_PTR(err));
		return 0;
	}

	for (i = 0; i < 3; i++) {
		err = sdei_event_unregister(adata->sdei_event);
		if (err != -EINPROGRESS)
			break;

		schedule();
	}

	if (err)
		dev_err(&pdev->dev, "Failed to unregister sdei-event #%d (%pe)\n",
			adata->sdei_event, ERR_PTR(err));

	return 0;
}

static struct platform_driver agdi_driver = {
	.driver = {
		.name = "agdi",
	},
	.probe = agdi_probe,
	.remove = agdi_remove,
};

void __init acpi_agdi_init(void)
{
	struct acpi_table_agdi *agdi_table;
	struct agdi_data pdata;
	struct platform_device *pdev;
	acpi_status status;

	status = acpi_get_table(ACPI_SIG_AGDI, 0,
				(struct acpi_table_header **) &agdi_table);
	if (ACPI_FAILURE(status))
		return;

	if (agdi_table->flags & ACPI_AGDI_SIGNALING_MODE) {
		pr_warn("Interrupt signaling is not supported");
		goto err_put_table;
	}

	pdata.sdei_event = agdi_table->sdei_event;

	pdev = platform_device_register_data(NULL, "agdi", 0, &pdata, sizeof(pdata));
	if (IS_ERR(pdev))
		goto err_put_table;

	if (platform_driver_register(&agdi_driver))
		platform_device_unregister(pdev);

err_put_table:
	acpi_put_table((struct acpi_table_header *)agdi_table);
}
