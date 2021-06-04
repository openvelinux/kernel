#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/workqueue.h>
#include <linux/percpu.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/seqlock.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <asm/irq_regs.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <linux/sched/task_stack.h>
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("BYTEDANCE STE");
MODULE_DESCRIPTION("Bytedance IDC workload hot component inspector");
MODULE_VERSION("0.12");

struct snap_entry {
	u64 ts;
	u16 cpu;
	u16 r1;
	u16 state1;
	u16 state2;
	u32 pid;
	u32 tid;
	u64 eip;
	char comm[TASK_COMM_LEN];
	/* new fields since v2 */
	u64 kernel_eip;
	u64 r2;
};

static u32 timer_ms = 10000;

static DEFINE_PER_CPU(struct timer_list, stack_snap_timer);
static DEFINE_PER_CPU(struct snap_entry, snap_entries);
static DEFINE_PER_CPU(seqcount_t, entries_lock);

static struct dentry *dir;
static int hot_inspector_cpustate;

static ssize_t entries_read(struct file *file, char __user *buf,
                            size_t count, loff_t *ppos, loff_t avail)
{
	loff_t pos = *ppos;
	struct snap_entry *entry = (struct snap_entry*)file->private_data;
	int cpu = entry->cpu;
	int seq;
	int ret;
	seqcount_t *seqcount = per_cpu_ptr(&entries_lock, cpu);

	if (pos < 0)
		return -EINVAL;
	if (pos >= avail)
		return 0;
	if (count > avail - pos)
		count = avail - pos;

	do {
		seq = read_seqcount_begin(seqcount);

		ret = copy_to_user(buf, file->private_data + pos, count);
		if (ret)
			return -EFAULT;
	} while (read_seqcount_retry(seqcount, seq));

	*ppos = pos + count;
	return count;
}

static ssize_t entries_read_v1(struct file *file, char __user *buf,
                               size_t count, loff_t *ppos)
{
	/* v1: data layout only until comm field */
	loff_t avail = offsetofend(struct snap_entry, comm);
	return entries_read(file, buf, count, ppos, avail);
}

static ssize_t entries_read_v2(struct file *file, char __user *buf,
                               size_t count, loff_t *ppos)
{
	loff_t avail = file_inode(file)->i_size;
	return entries_read(file, buf, count, ppos, avail);
}

static const struct file_operations hot_inspector_fops = {
	.owner   = THIS_MODULE,
	.open    = simple_open,
	.read    = entries_read_v1,
	.llseek  = default_llseek,
};

static const struct file_operations hot_inspector_v2_fops = {
	.owner   = THIS_MODULE,
	.open    = simple_open,
	.read    = entries_read_v2,
	.llseek  = default_llseek,
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
static void stack_snap(unsigned long data)
{
	struct timer_list *timer = (struct timer_list *)data;
#else
static void stack_snap(struct timer_list *timer)
{
#endif
	struct task_struct *task = current;
	struct pt_regs *regs = get_irq_regs() ? : task_pt_regs(task);
	struct snap_entry *entry = this_cpu_ptr(&snap_entries);
	seqcount_t *seqcount = this_cpu_ptr(&entries_lock);
	u32 delta_ms = clamp(timer_ms, 100U, 1000000U);

	write_seqcount_begin(seqcount);
	entry->ts = ktime_to_ns(ktime_get_boottime());
	entry->r1 = task->flags & PF_KTHREAD;
	entry->state1 = task->state;
	entry->state2 = regs && user_mode(regs) ? 0 : 1;
	entry->pid = task->tgid;
	entry->tid = task->pid;
	entry->eip = KSTK_EIP(task);
	entry->kernel_eip = regs ? instruction_pointer(regs) : 0;
	memcpy(entry->comm, task->comm, TASK_COMM_LEN);
	write_seqcount_end(seqcount);

	mod_timer(timer, jiffies + msecs_to_jiffies(delta_ms));
}

static int hot_inspector_startup(unsigned int cpu)
{
	struct timer_list *timer;
	u32 delta_ms = clamp(timer_ms, 100U, 1000000U);

	timer = per_cpu_ptr(&stack_snap_timer, cpu);
	timer->expires = jiffies + msecs_to_jiffies(delta_ms);
	add_timer_on(timer, cpu);

	return 0;
}

static int hot_inspector_teardown(unsigned int cpu)
{
	struct timer_list *timer;

	timer = per_cpu_ptr(&stack_snap_timer, cpu);
	del_timer_sync(timer);

	return 0;
}

static int __init hot_inspector_init(void)
{
	int cpu;
	int ret;
	char buf[32];
	struct dentry *cpu_dir;
	struct dentry *v2_dir;

	dir = debugfs_create_dir("hot_inspector", NULL);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	cpu_dir = debugfs_create_dir("cpu", dir);
	if (IS_ERR(cpu_dir)) {
		ret = PTR_ERR(cpu_dir);
		goto clean;
	}

	v2_dir = debugfs_create_dir("v2", dir);
	if (IS_ERR(v2_dir)) {
		ret = PTR_ERR(v2_dir);
		goto clean;
	}

	debugfs_create_u32("timer_ms", 0600, dir, &timer_ms);

	for_each_possible_cpu(cpu) {
		seqcount_t *seqcount = per_cpu_ptr(&entries_lock, cpu);
		struct snap_entry *entry = per_cpu_ptr(&snap_entries, cpu);
		struct timer_list *timer;

		seqcount_init(seqcount);
		entry->cpu = cpu;
		entry->ts = 0;

		timer = per_cpu_ptr(&stack_snap_timer, cpu);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
		__setup_timer(timer, stack_snap, (unsigned long)timer,
		              TIMER_DEFERRABLE | TIMER_PINNED | TIMER_IRQSAFE);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
		timer->flags = TIMER_DEFERRABLE | TIMER_PINNED | TIMER_IRQSAFE;
		setup_timer(timer, stack_snap, (unsigned long)timer);
#else
		timer_setup(timer, stack_snap, TIMER_DEFERRABLE |
		            TIMER_PINNED | TIMER_IRQSAFE);
#endif

		snprintf(buf, sizeof(buf), "%d", cpu);
		debugfs_create_file_size(buf, 0400, cpu_dir, entry,
		                         &hot_inspector_fops,
		                         sizeof(struct snap_entry));
		debugfs_create_file_size(buf, 0400, v2_dir, entry,
		                         &hot_inspector_v2_fops,
		                         sizeof(struct snap_entry));
	}

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "hot_inspector:prepare",
	                        hot_inspector_startup, hot_inspector_teardown);
	if (ret <= 0)
		goto clean;

	hot_inspector_cpustate = ret;

	return 0;
clean:
	debugfs_remove_recursive(dir);
	return ret;
}

static void __exit hot_inspector_exit(void)
{
	debugfs_remove_recursive(dir);
	cpuhp_remove_state(hot_inspector_cpustate);
}

module_init(hot_inspector_init);
module_exit(hot_inspector_exit);
