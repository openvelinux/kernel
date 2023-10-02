// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CPU Microcode Update Driver for Linux
 *
 * Copyright (C) 2000-2006 Tigran Aivazian <aivazian.tigran@gmail.com>
 *	      2006	Shaohua Li <shaohua.li@intel.com>
 *	      2013-2016	Borislav Petkov <bp@alien8.de>
 *
 * X86 CPU microcode early update for Linux:
 *
 *	Copyright (C) 2012 Fenghua Yu <fenghua.yu@intel.com>
 *			   H Peter Anvin" <hpa@zytor.com>
 *		  (C) 2015 Borislav Petkov <bp@alien8.de>
 *
 * This driver allows to upgrade microcode on x86 processors.
 */

#define pr_fmt(fmt) "microcode: " fmt

#include <linux/platform_device.h>
#include <linux/stop_machine.h>
#include <linux/syscore_ops.h>
#include <linux/miscdevice.h>
#include <linux/capability.h>
#include <linux/firmware.h>
#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/cpu.h>
#include <linux/nmi.h>
#include <linux/fs.h>
#include <linux/mm.h>

#include <asm/apic.h>
#include <asm/microcode_intel.h>
#include <asm/cpu_device_id.h>
#include <asm/microcode_amd.h>
#include <asm/perf_event.h>
#include <asm/microcode.h>
#include <asm/processor.h>
#include <asm/cmdline.h>
#include <asm/setup.h>

#define DRIVER_VERSION	"2.2"

static struct microcode_ops	*microcode_ops;
static bool dis_ucode_ldr = true;

bool initrd_gone;

LIST_HEAD(microcode_cache);

/*
 * Synchronization.
 *
 * All non cpu-hotplug-callback call sites use:
 *
 * - microcode_mutex to synchronize with each other;
 * - cpus_read_lock/unlock() to synchronize with
 *   the cpu-hotplug-callback call sites.
 *
 * We guarantee that only a single cpu is being
 * updated at any particular moment of time.
 */
static DEFINE_MUTEX(microcode_mutex);

struct ucode_cpu_info		ucode_cpu_info[NR_CPUS];

struct cpu_info_ctx {
	struct cpu_signature	*cpu_sig;
	int			err;
};

/*
 * Those patch levels cannot be updated to newer ones and thus should be final.
 */
static u32 final_levels[] = {
	0x01000098,
	0x0100009f,
	0x010000af,
	0, /* T-101 terminator */
};

/*
 * Check the current patch level on this CPU.
 *
 * Returns:
 *  - true: if update should stop
 *  - false: otherwise
 */
static bool amd_check_current_patch_level(void)
{
	u32 lvl, dummy, i;
	u32 *levels;

	native_rdmsr(MSR_AMD64_PATCH_LEVEL, lvl, dummy);

	if (IS_ENABLED(CONFIG_X86_32))
		levels = (u32 *)__pa_nodebug(&final_levels);
	else
		levels = final_levels;

	for (i = 0; levels[i]; i++) {
		if (lvl == levels[i])
			return true;
	}
	return false;
}

static bool __init check_loader_disabled_bsp(void)
{
	static const char *__dis_opt_str = "dis_ucode_ldr";

#ifdef CONFIG_X86_32
	const char *cmdline = (const char *)__pa_nodebug(boot_command_line);
	const char *option  = (const char *)__pa_nodebug(__dis_opt_str);
	bool *res = (bool *)__pa_nodebug(&dis_ucode_ldr);

#else /* CONFIG_X86_64 */
	const char *cmdline = boot_command_line;
	const char *option  = __dis_opt_str;
	bool *res = &dis_ucode_ldr;
#endif

	/*
	 * CPUID(1).ECX[31]: reserved for hypervisor use. This is still not
	 * completely accurate as xen pv guests don't see that CPUID bit set but
	 * that's good enough as they don't land on the BSP path anyway.
	 */
	if (native_cpuid_ecx(1) & BIT(31))
		return *res;

	if (x86_cpuid_vendor() == X86_VENDOR_AMD) {
		if (amd_check_current_patch_level())
			return *res;
	}

	if (cmdline_find_option_bool(cmdline, option) <= 0)
		*res = false;

	return *res;
}

extern struct builtin_fw __start_builtin_fw[];
extern struct builtin_fw __end_builtin_fw[];

bool get_builtin_firmware(struct cpio_data *cd, const char *name)
{
	struct builtin_fw *b_fw;

	for (b_fw = __start_builtin_fw; b_fw != __end_builtin_fw; b_fw++) {
		if (!strcmp(name, b_fw->name)) {
			cd->size = b_fw->size;
			cd->data = b_fw->data;
			return true;
		}
	}
	return false;
}

void __init load_ucode_bsp(void)
{
	unsigned int cpuid_1_eax;
	bool intel = true;

	if (!have_cpuid_p())
		return;

	cpuid_1_eax = native_cpuid_eax(1);

	switch (x86_cpuid_vendor()) {
	case X86_VENDOR_INTEL:
		if (x86_family(cpuid_1_eax) < 6)
			return;
		break;

	case X86_VENDOR_AMD:
		if (x86_family(cpuid_1_eax) < 0x10)
			return;
		intel = false;
		break;

	default:
		return;
	}

	if (check_loader_disabled_bsp())
		return;

	if (intel)
		load_ucode_intel_bsp();
	else
		load_ucode_amd_early(cpuid_1_eax);
}

static bool check_loader_disabled_ap(void)
{
#ifdef CONFIG_X86_32
	return *((bool *)__pa_nodebug(&dis_ucode_ldr));
#else
	return dis_ucode_ldr;
#endif
}

void load_ucode_ap(void)
{
	unsigned int cpuid_1_eax;

	if (check_loader_disabled_ap())
		return;

	cpuid_1_eax = native_cpuid_eax(1);

	switch (x86_cpuid_vendor()) {
	case X86_VENDOR_INTEL:
		if (x86_family(cpuid_1_eax) >= 6)
			load_ucode_intel_ap();
		break;
	case X86_VENDOR_AMD:
		if (x86_family(cpuid_1_eax) >= 0x10)
			load_ucode_amd_early(cpuid_1_eax);
		break;
	default:
		break;
	}
}

static int __init save_microcode_in_initrd(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;
	int ret = -EINVAL;

	switch (c->x86_vendor) {
	case X86_VENDOR_INTEL:
		if (c->x86 >= 6)
			ret = save_microcode_in_initrd_intel();
		break;
	case X86_VENDOR_AMD:
		if (c->x86 >= 0x10)
			ret = save_microcode_in_initrd_amd(cpuid_eax(1));
		break;
	default:
		break;
	}

	initrd_gone = true;

	return ret;
}

struct cpio_data find_microcode_in_initrd(const char *path, bool use_pa)
{
#ifdef CONFIG_BLK_DEV_INITRD
	unsigned long start = 0;
	size_t size;

#ifdef CONFIG_X86_32
	struct boot_params *params;

	if (use_pa)
		params = (struct boot_params *)__pa_nodebug(&boot_params);
	else
		params = &boot_params;

	size = params->hdr.ramdisk_size;

	/*
	 * Set start only if we have an initrd image. We cannot use initrd_start
	 * because it is not set that early yet.
	 */
	if (size)
		start = params->hdr.ramdisk_image;

# else /* CONFIG_X86_64 */
	size  = (unsigned long)boot_params.ext_ramdisk_size << 32;
	size |= boot_params.hdr.ramdisk_size;

	if (size) {
		start  = (unsigned long)boot_params.ext_ramdisk_image << 32;
		start |= boot_params.hdr.ramdisk_image;

		start += PAGE_OFFSET;
	}
# endif

	/*
	 * Fixup the start address: after reserve_initrd() runs, initrd_start
	 * has the virtual address of the beginning of the initrd. It also
	 * possibly relocates the ramdisk. In either case, initrd_start contains
	 * the updated address so use that instead.
	 *
	 * initrd_gone is for the hotplug case where we've thrown out initrd
	 * already.
	 */
	if (!use_pa) {
		if (initrd_gone)
			return (struct cpio_data){ NULL, 0, "" };
		if (initrd_start)
			start = initrd_start;
	} else {
		/*
		 * The picture with physical addresses is a bit different: we
		 * need to get the *physical* address to which the ramdisk was
		 * relocated, i.e., relocated_ramdisk (not initrd_start) and
		 * since we're running from physical addresses, we need to access
		 * relocated_ramdisk through its *physical* address too.
		 */
		u64 *rr = (u64 *)__pa_nodebug(&relocated_ramdisk);
		if (*rr)
			start = *rr;
	}

	return find_cpio_data(path, (void *)start, size, NULL);
#else /* !CONFIG_BLK_DEV_INITRD */
	return (struct cpio_data){ NULL, 0, "" };
#endif
}

void reload_early_microcode(unsigned int cpu)
{
	int vendor, family;

	vendor = x86_cpuid_vendor();
	family = x86_cpuid_family();

	switch (vendor) {
	case X86_VENDOR_INTEL:
		if (family >= 6)
			reload_ucode_intel();
		break;
	case X86_VENDOR_AMD:
		if (family >= 0x10)
			reload_ucode_amd(cpu);
		break;
	default:
		break;
	}
}

static void collect_cpu_info_local(void *arg)
{
	struct cpu_info_ctx *ctx = arg;

	ctx->err = microcode_ops->collect_cpu_info(smp_processor_id(),
						   ctx->cpu_sig);
}

static int collect_cpu_info_on_target(int cpu, struct cpu_signature *cpu_sig)
{
	struct cpu_info_ctx ctx = { .cpu_sig = cpu_sig, .err = 0 };
	int ret;

	ret = smp_call_function_single(cpu, collect_cpu_info_local, &ctx, 1);
	if (!ret)
		ret = ctx.err;

	return ret;
}

static int collect_cpu_info(int cpu)
{
	struct ucode_cpu_info *uci = ucode_cpu_info + cpu;
	int ret;

	memset(uci, 0, sizeof(*uci));

	ret = collect_cpu_info_on_target(cpu, &uci->cpu_sig);
	if (!ret)
		uci->valid = 1;

	return ret;
}

static void apply_microcode_local(void *arg)
{
	enum ucode_state *err = arg;

	*err = microcode_ops->apply_microcode(smp_processor_id());
}

static int apply_microcode_on_target(int cpu)
{
	enum ucode_state err;
	int ret;

	ret = smp_call_function_single(cpu, apply_microcode_local, &err, 1);
	if (!ret) {
		if (err == UCODE_ERROR)
			ret = 1;
	}
	return ret;
}

/* fake device for request_firmware */
static struct platform_device	*microcode_pdev;

#ifdef CONFIG_MICROCODE_LATE_LOADING
/*
 * Late loading dance. Why the heavy-handed stomp_machine effort?
 *
 * - HT siblings must be idle and not execute other code while the other sibling
 *   is loading microcode in order to avoid any negative interactions caused by
 *   the loading.
 *
 * - In addition, microcode update on the cores must be serialized until this
 *   requirement can be relaxed in the future. Right now, this is conservative
 *   and good.
 */
enum sibling_ctrl {
	/* Spinwait with timeout */
	SCTRL_WAIT,
	/* Invoke the microcode_apply() callback */
	SCTRL_APPLY,
	/* Proceed without invoking the microcode_apply() callback */
	SCTRL_DONE,
};

struct microcode_ctrl {
	enum sibling_ctrl	ctrl;
	enum ucode_state	result;
	unsigned int		ctrl_cpu;
	bool			nmi_enabled;
};

DEFINE_STATIC_KEY_FALSE(microcode_nmi_handler_enable);
static DEFINE_PER_CPU(struct microcode_ctrl, ucode_ctrl);
static atomic_t late_cpus_in;

static bool wait_for_cpus(atomic_t *cnt)
{
	unsigned int timeout;

	WARN_ON_ONCE(atomic_dec_return(cnt) < 0);

	for (timeout = 0; timeout < USEC_PER_SEC; timeout++) {
		if (!atomic_read(cnt))
			return true;

		udelay(1);

		/* If invoked directly, tickle the NMI watchdog */
		if (!microcode_ops->use_nmi && !(timeout % USEC_PER_MSEC))
			touch_nmi_watchdog();
	}
	/* Prevent the late comers from making progress and let them time out */
	atomic_inc(cnt);
	return false;
}

static bool wait_for_ctrl(void)
{
	unsigned int timeout;

	for (timeout = 0; timeout < USEC_PER_SEC; timeout++) {
		if (this_cpu_read(ucode_ctrl.ctrl) != SCTRL_WAIT)
			return true;
		udelay(1);
		/* If invoked directly, tickle the NMI watchdog */
		if (!microcode_ops->use_nmi && !(timeout % 1000))
			touch_nmi_watchdog();
	}
	return false;
}

static void load_secondary(unsigned int cpu)
{
	unsigned int ctrl_cpu = this_cpu_read(ucode_ctrl.ctrl_cpu);
	enum ucode_state ret;

	/* Initial rendezvous to ensure that all CPUs have arrived */
	if (!wait_for_cpus(&late_cpus_in)) {
		pr_err_once("load: %d CPUs timed out\n", atomic_read(&late_cpus_in) - 1);
		this_cpu_write(ucode_ctrl.result, UCODE_TIMEOUT);
		return;
	}

	/*
	 * Wait for primary threads to complete. If one of them hangs due
	 * to the update, there is no way out. This is non-recoverable
	 * because the CPU might hold locks or resources and confuse the
	 * scheduler, watchdogs etc. There is no way to safely evacuate the
	 * machine.
	 */
	if (!wait_for_ctrl())
		panic("Microcode load: Primary CPU %d timed out\n", ctrl_cpu);

	/*
	 * If the primary succeeded then invoke the apply() callback,
	 * otherwise copy the state from the primary thread.
	 */
	if (this_cpu_read(ucode_ctrl.ctrl) == SCTRL_APPLY)
		ret = microcode_ops->apply_microcode(cpu);
	else
		ret = per_cpu(ucode_ctrl.result, ctrl_cpu);

	this_cpu_write(ucode_ctrl.result, ret);
	this_cpu_write(ucode_ctrl.ctrl, SCTRL_DONE);
}

static void load_primary(unsigned int cpu)
{
	struct cpumask *secondaries = topology_sibling_cpumask(cpu);
	enum sibling_ctrl ctrl;
	enum ucode_state ret;
	unsigned int sibling;

	/* Initial rendezvous to ensure that all CPUs have arrived */
	if (!wait_for_cpus(&late_cpus_in)) {
		this_cpu_write(ucode_ctrl.result, UCODE_TIMEOUT);
		pr_err_once("load: %d CPUs timed out\n", atomic_read(&late_cpus_in) - 1);
		return;
	}

	ret = microcode_ops->apply_microcode(cpu);
	this_cpu_write(ucode_ctrl.result, ret);
	this_cpu_write(ucode_ctrl.ctrl, SCTRL_DONE);

	/*
	 * If the update was successful, let the siblings run the apply()
	 * callback. If not, tell them it's done. This also covers the
	 * case where the CPU has uniform loading at package or system
	 * scope implemented but does not advertise it.
	 */
	if (ret == UCODE_UPDATED || ret == UCODE_OK)
		ctrl = SCTRL_APPLY;
	else
		ctrl = SCTRL_DONE;

	for_each_cpu(sibling, secondaries) {
		if (sibling != cpu)
			per_cpu(ucode_ctrl.ctrl, sibling) = ctrl;
	}
}

static bool microcode_update_handler(void)
{
	unsigned int cpu = smp_processor_id();

	if (this_cpu_read(ucode_ctrl.ctrl_cpu) == cpu)
		load_primary(cpu);
	else
		load_secondary(cpu);

	touch_nmi_watchdog();
	return true;
}

bool microcode_nmi_handler(void)
{
	if (!this_cpu_read(ucode_ctrl.nmi_enabled))
		return false;

	this_cpu_write(ucode_ctrl.nmi_enabled, false);
	return microcode_update_handler();
}

static int load_cpus_stopped(void *unused)
{
	if (microcode_ops->use_nmi) {
		/* Enable the NMI handler and raise NMI */
		this_cpu_write(ucode_ctrl.nmi_enabled, true);
		apic->send_IPI(smp_processor_id(), NMI_VECTOR);
	} else {
		/* Just invoke the handler directly */
		microcode_update_handler();
	}
	return 0;
}

static int load_late_stop_cpus(void)
{
	unsigned int cpu, updated = 0, failed = 0, timedout = 0, siblings = 0;
	int old_rev = boot_cpu_data.microcode;
	struct cpuinfo_x86 prev_info;

	atomic_set(&late_cpus_in, num_online_cpus());

	/*
	 * Take a snapshot before the microcode update in order to compare and
	 * check whether any bits changed after an update.
	 */
	store_cpu_caps(&prev_info);

	if (microcode_ops->use_nmi)
		static_branch_enable_cpuslocked(&microcode_nmi_handler_enable);

	stop_machine_cpuslocked(load_cpus_stopped, NULL, cpu_online_mask);

	if (microcode_ops->use_nmi)
		static_branch_disable_cpuslocked(&microcode_nmi_handler_enable);

	/* Analyze the results */
	for_each_cpu_and(cpu, cpu_present_mask, &cpus_booted_once_mask) {
		switch (per_cpu(ucode_ctrl.result, cpu)) {
		case UCODE_UPDATED:	updated++; break;
		case UCODE_TIMEOUT:	timedout++; break;
		case UCODE_OK:		siblings++; break;
		default:		failed++; break;
		}
	}

	if (microcode_ops->finalize_late_load)
		microcode_ops->finalize_late_load(!updated);

	if (!updated) {
		/* Nothing changed. */
		if (!failed && !timedout)
			return 0;
		pr_err("update failed: %u CPUs failed %u CPUs timed out\n",
		       failed, timedout);
		return -EIO;
	}

	add_taint(TAINT_CPU_OUT_OF_SPEC, LOCKDEP_STILL_OK);
	pr_info("load: updated on %u primary CPUs with %u siblings\n", updated, siblings);
	if (failed || timedout) {
		pr_err("load incomplete. %u CPUs timed out or failed\n",
		       num_online_cpus() - (updated + siblings));
	}
	pr_info("revision: 0x%x -> 0x%x\n", old_rev, boot_cpu_data.microcode);
	microcode_check(&prev_info);

	return updated + siblings == num_online_cpus() ? 0 : -EIO;
}

/*
 * This function does two things:
 *
 * 1) Ensure that all required CPUs which are present and have been booted
 *    once are online.
 *
 *    To pass this check, all primary threads must be online.
 *
 *    If the microcode load is not safe against NMI then all SMT threads
 *    must be online as well because they still react to NMIs when they are
 *    soft-offlined and parked in one of the play_dead() variants. So if a
 *    NMI hits while the primary thread updates the microcode the resulting
 *    behaviour is undefined. The default play_dead() implementation on
 *    modern CPUs uses MWAIT, which is also not guaranteed to be safe
 *    against a microcode update which affects MWAIT.
 *
 * 2) Initialize the per CPU control structure
 */
static bool setup_cpus(void)
{
	struct microcode_ctrl ctrl = { .ctrl = SCTRL_WAIT, .result = -1, };
	unsigned int cpu;

	for_each_cpu_and(cpu, cpu_present_mask, &cpus_booted_once_mask) {
		if (!cpu_online(cpu)) {
			if (topology_is_primary_thread(cpu) || !microcode_ops->nmi_safe) {
				pr_err("CPU %u not online\n", cpu);
				return false;
			}
		}

		/*
		 * Initialize the per CPU state. This is core scope for now,
		 * but prepared to take package or system scope into account.
		 */
		ctrl.ctrl_cpu = cpumask_first(topology_sibling_cpumask(cpu));
		per_cpu(ucode_ctrl, cpu) = ctrl;
	}
	return true;
}

static int load_late_locked(void)
{
	if (!setup_cpus())
		return -EBUSY;

	switch (microcode_ops->request_microcode_fw(0, &microcode_pdev->dev)) {
	case UCODE_NEW:
		return load_late_stop_cpus();
	case UCODE_NFOUND:
		return -ENOENT;
	default:
		return -EBADFD;
	}
}

static ssize_t reload_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t size)
{
	unsigned long val;
	ssize_t ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	if (val != 1)
		return size;

	cpus_read_lock();
	ret = load_late_locked();
	cpus_read_unlock();

	return ret ? : size;
}

static DEVICE_ATTR_WO(reload);
#endif

static ssize_t version_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct ucode_cpu_info *uci = ucode_cpu_info + dev->id;

	return sprintf(buf, "0x%x\n", uci->cpu_sig.rev);
}

static ssize_t pf_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct ucode_cpu_info *uci = ucode_cpu_info + dev->id;

	return sprintf(buf, "0x%x\n", uci->cpu_sig.pf);
}

static DEVICE_ATTR(version, 0444, version_show, NULL);
static DEVICE_ATTR(processor_flags, 0444, pf_show, NULL);

static struct attribute *mc_default_attrs[] = {
	&dev_attr_version.attr,
	&dev_attr_processor_flags.attr,
	NULL
};

static const struct attribute_group mc_attr_group = {
	.attrs			= mc_default_attrs,
	.name			= "microcode",
};

static void microcode_fini_cpu(int cpu)
{
	if (microcode_ops->microcode_fini_cpu)
		microcode_ops->microcode_fini_cpu(cpu);
}

static enum ucode_state microcode_resume_cpu(int cpu)
{
	if (apply_microcode_on_target(cpu))
		return UCODE_ERROR;

	pr_debug("CPU%d updated upon resume\n", cpu);

	return UCODE_OK;
}

static enum ucode_state microcode_init_cpu(int cpu, bool refresh_fw)
{
	enum ucode_state ustate;
	struct ucode_cpu_info *uci = ucode_cpu_info + cpu;

	if (uci->valid)
		return UCODE_OK;

	if (collect_cpu_info(cpu))
		return UCODE_ERROR;

	/* --dimm. Trigger a delayed update? */
	if (system_state != SYSTEM_RUNNING)
		return UCODE_NFOUND;

	ustate = microcode_ops->request_microcode_fw(cpu, &microcode_pdev->dev);
	if (ustate == UCODE_NEW) {
		pr_debug("CPU%d updated upon init\n", cpu);
		apply_microcode_on_target(cpu);
	}

	return ustate;
}

static enum ucode_state microcode_update_cpu(int cpu)
{
	struct ucode_cpu_info *uci = ucode_cpu_info + cpu;

	/* Refresh CPU microcode revision after resume. */
	collect_cpu_info(cpu);

	if (uci->valid)
		return microcode_resume_cpu(cpu);

	return microcode_init_cpu(cpu, false);
}

static int mc_device_add(struct device *dev, struct subsys_interface *sif)
{
	int err, cpu = dev->id;

	if (!cpu_online(cpu))
		return 0;

	pr_debug("CPU%d added\n", cpu);

	err = sysfs_create_group(&dev->kobj, &mc_attr_group);
	if (err)
		return err;

	if (microcode_init_cpu(cpu, true) == UCODE_ERROR)
		return -EINVAL;

	return err;
}

static void mc_device_remove(struct device *dev, struct subsys_interface *sif)
{
	int cpu = dev->id;

	if (!cpu_online(cpu))
		return;

	pr_debug("CPU%d removed\n", cpu);
	microcode_fini_cpu(cpu);
	sysfs_remove_group(&dev->kobj, &mc_attr_group);
}

static struct subsys_interface mc_cpu_interface = {
	.name			= "microcode",
	.subsys			= &cpu_subsys,
	.add_dev		= mc_device_add,
	.remove_dev		= mc_device_remove,
};

/**
 * microcode_bsp_resume - Update boot CPU microcode during resume.
 */
void microcode_bsp_resume(void)
{
	int cpu = smp_processor_id();
	struct ucode_cpu_info *uci = ucode_cpu_info + cpu;

	if (uci->valid && uci->mc)
		microcode_ops->apply_microcode(cpu);
	else if (!uci->mc)
		reload_early_microcode(cpu);
}

static struct syscore_ops mc_syscore_ops = {
	.resume			= microcode_bsp_resume,
};

static int mc_cpu_starting(unsigned int cpu)
{
	microcode_update_cpu(cpu);
	pr_debug("CPU%d added\n", cpu);
	return 0;
}

static int mc_cpu_online(unsigned int cpu)
{
	struct device *dev = get_cpu_device(cpu);

	if (sysfs_create_group(&dev->kobj, &mc_attr_group))
		pr_err("Failed to create group for CPU%d\n", cpu);
	return 0;
}

static int mc_cpu_down_prep(unsigned int cpu)
{
	struct device *dev;

	dev = get_cpu_device(cpu);
	/* Suspend is in progress, only remove the interface */
	sysfs_remove_group(&dev->kobj, &mc_attr_group);
	pr_debug("CPU%d removed\n", cpu);

	return 0;
}

static struct attribute *cpu_root_microcode_attrs[] = {
#ifdef CONFIG_MICROCODE_LATE_LOADING
	&dev_attr_reload.attr,
#endif
	NULL
};

static const struct attribute_group cpu_root_microcode_group = {
	.name  = "microcode",
	.attrs = cpu_root_microcode_attrs,
};

static int __init microcode_init(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;
	int error;

	if (dis_ucode_ldr)
		return -EINVAL;

	if (c->x86_vendor == X86_VENDOR_INTEL)
		microcode_ops = init_intel_microcode();
	else if (c->x86_vendor == X86_VENDOR_AMD)
		microcode_ops = init_amd_microcode();
	else
		pr_err("no support for this CPU vendor\n");

	if (!microcode_ops)
		return -ENODEV;

	microcode_pdev = platform_device_register_simple("microcode", -1,
							 NULL, 0);
	if (IS_ERR(microcode_pdev))
		return PTR_ERR(microcode_pdev);

	cpus_read_lock();
	mutex_lock(&microcode_mutex);

	error = subsys_interface_register(&mc_cpu_interface);
	if (!error)
		perf_check_microcode();
	mutex_unlock(&microcode_mutex);
	cpus_read_unlock();

	if (error)
		goto out_pdev;

	error = sysfs_create_group(&cpu_subsys.dev_root->kobj,
				   &cpu_root_microcode_group);

	if (error) {
		pr_err("Error creating microcode group!\n");
		goto out_driver;
	}

	register_syscore_ops(&mc_syscore_ops);
	cpuhp_setup_state_nocalls(CPUHP_AP_MICROCODE_LOADER, "x86/microcode:starting",
				  mc_cpu_starting, NULL);
	cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "x86/microcode:online",
				  mc_cpu_online, mc_cpu_down_prep);

	pr_info("Microcode Update Driver: v%s.", DRIVER_VERSION);

	return 0;

 out_driver:
	cpus_read_lock();
	mutex_lock(&microcode_mutex);

	subsys_interface_unregister(&mc_cpu_interface);

	mutex_unlock(&microcode_mutex);
	cpus_read_unlock();

 out_pdev:
	platform_device_unregister(microcode_pdev);
	return error;

}
fs_initcall(save_microcode_in_initrd);
late_initcall(microcode_init);
