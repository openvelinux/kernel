// SPDX-License-Identifier: GPL-2.0
/**
 * Code for trace irq or bh off.
 */
#include <linux/hrtimer.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/proc_fs.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/stacktrace.h>
#include <linux/timer.h>
#include <asm/irq_regs.h>

#ifndef MODULE
#define MAX_TRACE_ENTRIES		(SZ_4K / sizeof(unsigned long))
#define PER_TRACE_ENTRIES_AVERAGE	8
#else
/**
 * If we compile as a module, the static per cpu varibles will be
 * dynamically allocated via alloc_percpu(). If requset memory size
 * exceeds PCPU_MIN_UNIT_SIZE, we will fail to allocate the memory
 * and result in insmod module failed.
 */
#define MAX_TRACE_ENTRIES		(SZ_1K / sizeof(unsigned long))
#define PER_TRACE_ENTRIES_AVERAGE	(8 + 8)
#endif

#define MAX_STACE_TRACE_ENTRIES		\
	(MAX_TRACE_ENTRIES / PER_TRACE_ENTRIES_AVERAGE)

#define MAX_LATENCY_RECORD		10

static bool trace_enable;

/**
 * Default sampling period is 10000000ns. The minimum value is 1000000ns.
 */
static u64 sampling_period = 10 * 1000 * 1000UL;

/**
 * How many times should we record the stack trace.
 * Default is 50000000ns.
 */
static u64 trace_irqoff_latency = 50 * 1000 * 1000UL;

struct stack_trace_metadata {
	u64 last_timestamp;
	unsigned long nr_stack_trace;
	struct stack_trace trace[MAX_STACE_TRACE_ENTRIES];
	unsigned long nr_entries;
	unsigned long entries[MAX_TRACE_ENTRIES];
	unsigned long latency_count[MAX_LATENCY_RECORD];

	/* Task command names*/
	char comms[MAX_STACE_TRACE_ENTRIES][TASK_COMM_LEN];

	/* Task pids*/
	pid_t pids[MAX_STACE_TRACE_ENTRIES];
	u64 latency[MAX_STACE_TRACE_ENTRIES];
};

struct per_cpu_stack_trace {
	struct timer_list timer;
	struct hrtimer hrtimer;
	struct stack_trace_metadata hardirq_trace;
	struct stack_trace_metadata softirq_trace;

	bool softirq_delayed;
};

static DEFINE_PER_CPU(struct per_cpu_stack_trace, cpu_stack_trace);

/**
 * Note: Must be called with irq disabled.
 */
static bool save_trace(struct pt_regs *regs, bool hardirq, u64 latency)
{
	unsigned long nr_entries, nr_stack_trace;
	struct stack_trace *trace;
	struct stack_trace_metadata *stack_trace;

	stack_trace = hardirq ? this_cpu_ptr(&cpu_stack_trace.hardirq_trace) :
		      this_cpu_ptr(&cpu_stack_trace.softirq_trace);

	nr_stack_trace = stack_trace->nr_stack_trace;
	if (unlikely(nr_stack_trace >= MAX_STACE_TRACE_ENTRIES))
		return false;

	nr_entries = stack_trace->nr_entries;
	if (unlikely(nr_entries >= MAX_TRACE_ENTRIES - 1))
		return false;

	strlcpy(stack_trace->comms[nr_stack_trace], current->comm,
		TASK_COMM_LEN);
	stack_trace->pids[nr_stack_trace] = current->pid;
	stack_trace->latency[nr_stack_trace] = latency;

	trace = stack_trace->trace + nr_stack_trace;
	trace->nr_entries = 0;
	trace->max_entries = MAX_TRACE_ENTRIES - nr_entries;
	trace->entries = stack_trace->entries + nr_entries;
	trace->skip = 0;

#ifndef MODULE
	save_stack_trace_regs(regs, trace);
#else
	save_stack_trace(trace);
#endif
	/*
	 * Some daft arches put -1 at the end to indicate its a full trace.
	 *
	 * <rant> this is buggy anyway, since it takes a whole extra entry so a
	 * complete trace that maxes out the entries provided will be reported
	 * as incomplete, friggin useless </rant>.
	 */
	if (trace->nr_entries != 0 &&
	    trace->entries[trace->nr_entries - 1] == ULONG_MAX)
		trace->nr_entries--;

	trace->max_entries = trace->nr_entries;

	stack_trace->nr_entries += trace->nr_entries;

	/**
	 * Ensure that the initialisation of @trace is complete before we
	 * update the @nr_stack_trace.
	 */
	smp_store_release(&stack_trace->nr_stack_trace, nr_stack_trace + 1);

	if (unlikely(stack_trace->nr_entries >= MAX_TRACE_ENTRIES - 1)) {
		pr_info("BUG: MAX_TRACE_ENTRIES too low!");
		dump_stack();

		return false;
	}

	return true;
}

static bool trace_irqoff_record(u64 delta, bool hardirq)
{
	int index = 0;
	u64 throttle = sampling_period << 1;
	u64 delta_old = delta;

	if (delta < throttle)
		return false;

	delta >>= 1;
	while (delta > throttle) {
		index++;
		delta >>= 1;
	}

	if (unlikely(index >= MAX_LATENCY_RECORD))
		index = MAX_LATENCY_RECORD - 1;

	if (hardirq)
		__this_cpu_inc(cpu_stack_trace.hardirq_trace.latency_count[index]);
	else
		__this_cpu_inc(cpu_stack_trace.softirq_trace.latency_count[index]);

	if (unlikely(delta_old >= trace_irqoff_latency))
		save_trace(get_irq_regs(), hardirq, delta_old);

	return true;
}

static enum hrtimer_restart trace_irqoff_hrtimer_handler(struct hrtimer *hrtimer)
{
	u64 now = local_clock(), delta;

	delta = now - __this_cpu_read(cpu_stack_trace.hardirq_trace.last_timestamp);
	__this_cpu_write(cpu_stack_trace.hardirq_trace.last_timestamp, now);

	if (trace_irqoff_record(delta, true)) {
		__this_cpu_write(cpu_stack_trace.softirq_trace.last_timestamp,
				 now);
	} else if (!__this_cpu_read(cpu_stack_trace.softirq_delayed)) {
		u64 delta_soft;

		delta_soft = now -
			__this_cpu_read(cpu_stack_trace.softirq_trace.last_timestamp);

		if (unlikely(delta_soft >= trace_irqoff_latency)) {
			__this_cpu_write(cpu_stack_trace.softirq_delayed,
					 true);
			trace_irqoff_record(delta_soft, false);
		}
	}

	hrtimer_forward_now(hrtimer, ns_to_ktime(sampling_period));

	return HRTIMER_RESTART;
}

static void trace_irqoff_timer_handler(struct timer_list *timer)
{
	u64 now = local_clock(), delta;

	delta = now - __this_cpu_read(cpu_stack_trace.softirq_trace.last_timestamp);
	__this_cpu_write(cpu_stack_trace.softirq_trace.last_timestamp, now);

	__this_cpu_write(cpu_stack_trace.softirq_delayed, false);

	trace_irqoff_record(delta, false);

	mod_timer(timer, jiffies + nsecs_to_jiffies(sampling_period));
}

static void smp_clear_stack_trace(void *info)
{
	int i;
	struct per_cpu_stack_trace *stack_trace;

	stack_trace = this_cpu_ptr(&cpu_stack_trace);

	stack_trace->hardirq_trace.nr_entries = 0;
	stack_trace->hardirq_trace.nr_stack_trace = 0;
	stack_trace->softirq_trace.nr_entries = 0;
	stack_trace->softirq_trace.nr_stack_trace = 0;

	for (i = 0; i < MAX_LATENCY_RECORD; i++) {
		stack_trace->hardirq_trace.latency_count[i] = 0;
		stack_trace->softirq_trace.latency_count[i] = 0;
	}
}

static void smp_timers_start(void *info)
{
	u64 now = local_clock();
	struct per_cpu_stack_trace *stack_trace = info;
	struct hrtimer *hrtimer = &stack_trace->hrtimer;
	struct timer_list *timer = &stack_trace->timer;

	smp_clear_stack_trace(NULL);

	stack_trace->hardirq_trace.last_timestamp = now;
	stack_trace->softirq_trace.last_timestamp = now;

	hrtimer_start_range_ns(hrtimer, ns_to_ktime(sampling_period),
			       sampling_period >> 3,
			       HRTIMER_MODE_REL_PINNED);

	timer->expires = jiffies + nsecs_to_jiffies(sampling_period);
	add_timer_on(timer, smp_processor_id());
}

static void distribute_show_one(struct seq_file *m, void *v, bool hardirq)
{
	int i, cpu;
	int scale = (sampling_period << 1) / (1000 * 1000UL);
	unsigned long latency_count[MAX_LATENCY_RECORD] = { 0 };

	for_each_online_cpu(cpu) {
		unsigned long *count;

		count = hardirq ?
			per_cpu_ptr(cpu_stack_trace.hardirq_trace.latency_count, cpu) :
			per_cpu_ptr(cpu_stack_trace.softirq_trace.latency_count, cpu);

		for (i = 0; i < MAX_LATENCY_RECORD; i++)
			latency_count[i] += count[i];
	}

	seq_puts(m, "scale(ms): ");
	for (i = 0; i < MAX_LATENCY_RECORD; i++)
		seq_printf(m, "%-6d ", scale << i);

	seq_putc(m, '\n');

	seq_puts(m, "count    : ");
	for (i = 0; i < MAX_LATENCY_RECORD; i++)
		seq_printf(m, "%-6lu ", latency_count[i]);

	seq_putc(m, '\n');
}

static int distribute_show(struct seq_file *m, void *v)
{
	seq_puts(m, "hardirq:\n");
	distribute_show_one(m, v, true);

	seq_putc(m, '\n');

	seq_puts(m, "softirq:\n");
	distribute_show_one(m, v, false);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(distribute);

static void seq_print_stack_trace(struct seq_file *m, struct stack_trace *trace)
{
	int i;

	if (WARN_ON(!trace->entries))
		return;

	for (i = 0; i < trace->nr_entries; i++)
		seq_printf(m, "%*c%pS\n", 5, ' ', (void *)trace->entries[i]);
}

static ssize_t trace_latency_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	unsigned long latency;

	if (kstrtoul_from_user(buf, count, 0, &latency))
		return -EINVAL;

	if (latency == 0) {
		int cpu;

		for_each_online_cpu(cpu)
			smp_call_function_single(cpu, smp_clear_stack_trace,
						 NULL, true);
		return count;
	} else if (latency < (sampling_period << 1) / (1000 * 1000UL))
		return -EINVAL;

	trace_irqoff_latency = latency * 1000 * 1000UL;

	return count;
}

static void trace_latency_show_one(struct seq_file *m, void *v, bool hardirq)
{
	int cpu;

	for_each_online_cpu(cpu) {
		int i;
		unsigned long nr_stack_trace;
		struct stack_trace_metadata *stack_trace;

		stack_trace = hardirq ?
			per_cpu_ptr(&cpu_stack_trace.hardirq_trace, cpu) :
			per_cpu_ptr(&cpu_stack_trace.softirq_trace, cpu);

		/**
		 * Paired with smp_store_release() in the save_trace().
		 */
		nr_stack_trace = smp_load_acquire(&stack_trace->nr_stack_trace);

		if (!nr_stack_trace)
			continue;

		seq_printf(m, " cpu: %d\n", cpu);

		for (i = 0; i < nr_stack_trace; i++) {
			struct stack_trace *trace = stack_trace->trace + i;

			seq_printf(m, "%*cCOMMAND: %s PID: %d LATENCY: %llums\n",
				   5, ' ', stack_trace->comms[i],
				   stack_trace->pids[i],
				   stack_trace->latency[i] / (1000 * 1000UL));
			seq_print_stack_trace(m, trace);
			seq_putc(m, '\n');
		}
	}
}

static int trace_latency_show(struct seq_file *m, void *v)
{
	seq_printf(m, "trace_irqoff_latency: %llu ms\n\n",
		   trace_irqoff_latency / (1000 * 1000UL));

	seq_puts(m, " hardirq:\n");
	trace_latency_show_one(m, v, true);

	seq_putc(m, '\n');

	seq_puts(m, " softirq:\n");
	trace_latency_show_one(m, v, false);

	return 0;
}

static int trace_latency_open(struct inode *inode, struct file *file)
{
	return single_open(file, trace_latency_show, inode->i_private);
}

static const struct file_operations trace_latency_fops = {
	.owner		= THIS_MODULE,
	.open		= trace_latency_open,
	.read		= seq_read,
	.write		= trace_latency_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int enable_show(struct seq_file *m, void *ptr)
{
	seq_printf(m, "%s\n", trace_enable ? "enabled" : "disabled");

	return 0;
}

static int enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, enable_show, inode->i_private);
}

static void trace_irqoff_start_timers(void)
{
	int cpu;

	for_each_online_cpu(cpu) {
		struct hrtimer *hrtimer;
		struct timer_list *timer;

		hrtimer = per_cpu_ptr(&cpu_stack_trace.hrtimer, cpu);
		hrtimer_init(hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_PINNED);
		hrtimer->function = trace_irqoff_hrtimer_handler;

		timer = per_cpu_ptr(&cpu_stack_trace.timer, cpu);
		timer_setup(timer, trace_irqoff_timer_handler,
			    TIMER_PINNED | TIMER_IRQSAFE);

		smp_call_function_single(cpu, smp_timers_start,
					 per_cpu_ptr(&cpu_stack_trace, cpu),
					 true);
	}
}

static void trace_irqoff_cancel_timers(void)
{
	int cpu;

	for_each_online_cpu(cpu) {
		struct hrtimer *hrtimer;
		struct timer_list *timer;

		hrtimer = per_cpu_ptr(&cpu_stack_trace.hrtimer, cpu);
		hrtimer_cancel(hrtimer);

		timer = per_cpu_ptr(&cpu_stack_trace.timer, cpu);
		del_timer_sync(timer);
	}
}

static ssize_t enable_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	bool enable;

	if (kstrtobool_from_user(buf, count, &enable))
		return -EINVAL;

	if (!!enable == !!trace_enable)
		return count;

	if (enable)
		trace_irqoff_start_timers();
	else
		trace_irqoff_cancel_timers();

	trace_enable = enable;

	return count;
}

static const struct file_operations enable_fops = {
	.open		= enable_open,
	.read		= seq_read,
	.write		= enable_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int sampling_period_show(struct seq_file *m, void *ptr)
{
	seq_printf(m, "%llums\n", sampling_period / (1000 * 1000UL));

	return 0;
}

static int sampling_period_open(struct inode *inode, struct file *file)
{
	return single_open(file, sampling_period_show, inode->i_private);
}

static ssize_t sampling_period_write(struct file *file, const char __user *buf,
				     size_t count, loff_t *ppos)
{
	unsigned long period;

	if (trace_enable)
		return -EINVAL;

	if (kstrtoul_from_user(buf, count, 0, &period))
		return -EINVAL;

	period *= 1000 * 1000UL;
	if (period > (trace_irqoff_latency >> 1))
		trace_irqoff_latency = period << 1;

	sampling_period = period;

	return count;
}

static const struct file_operations sampling_period_fops = {
	.open		= sampling_period_open,
	.read		= seq_read,
	.write		= sampling_period_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init trace_irqoff_init(void)
{
	struct proc_dir_entry *parent_dir;

	parent_dir = proc_mkdir("trace_irqoff", NULL);
	if (!parent_dir)
		return -ENOMEM;

	if (!proc_create("distribute", S_IRUSR, parent_dir, &distribute_fops))
		goto remove_trace_irqoff;

	if (!proc_create("trace_latency", S_IRUSR | S_IWUSR, parent_dir,
			 &trace_latency_fops))
		goto remove_distribute;

	if (!proc_create("enable", S_IRUSR | S_IWUSR, parent_dir, &enable_fops))
		goto remove_trace_latency;

	if (!proc_create("sampling_period", S_IRUSR | S_IWUSR, parent_dir,
			 &sampling_period_fops))
		goto remove_enable;

	return 0;
remove_enable:
	remove_proc_entry("enable", parent_dir);
remove_trace_latency:
	remove_proc_entry("trace_latency", parent_dir);
remove_distribute:
	remove_proc_entry("distribute", parent_dir);
remove_trace_irqoff:
	proc_remove(parent_dir);

	return -ENOMEM;
}

static void __exit trace_irqoff_exit(void)
{
	if (trace_enable)
		trace_irqoff_cancel_timers();

	remove_proc_entry("trace_irqoff/sampling_period", NULL);
	remove_proc_entry("trace_irqoff/enable", NULL);
	remove_proc_entry("trace_irqoff/trace_latency", NULL);
	remove_proc_entry("trace_irqoff/distribute", NULL);
	remove_proc_entry("trace_irqoff", NULL);
}

module_init(trace_irqoff_init);
module_exit(trace_irqoff_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Muchun Song <songmuchun@bytedance.com>");
