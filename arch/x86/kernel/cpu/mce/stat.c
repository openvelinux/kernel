// SPDX-License-Identifier: GPL-2.0-only
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/list.h>
#include <linux/mce.h>
#include <linux/mm.h>

struct mce_stat {
	bool cmci;
	int signal;
	pid_t pid;
	unsigned long addr;
	u64 time;
	char comm[TASK_COMM_LEN];
};
#define MAX_NR_RECORD 256
static struct mce_stat mcestat[MAX_NR_RECORD];
static atomic_t mce_records = ATOMIC_INIT(0);
static bool mcestat_enabled __read_mostly = true;

static int mcestat_enabled_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", mcestat_enabled);
	return 0;
}

static ssize_t
mcestat_enabled_write(struct file *file, const char __user *buf, size_t len,
		      loff_t *ppos)
{
	unsigned long val;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	ret = kstrtoul_from_user(buf, len, 0, &val);
	if (ret)
		return ret;

	mcestat_enabled = !!val;
	return len;
}

static int mcestat_enabled_open(struct inode *inode, struct file *file)
{
	return single_open(file, mcestat_enabled_show, NULL);
}

static const struct proc_ops mcestat_enabled_fops = {
	.proc_open	= mcestat_enabled_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_write	= mcestat_enabled_write,
	.proc_release	= single_release,
};

static void mcestat_reset(void)
{
	atomic_set(&mce_records, 0);
}

void mcestat_record(struct task_struct *task,
		    unsigned long addr, int signal, bool cmci)
{
	int records;

	if (!mcestat_enabled)
		return;

	records = atomic_inc_return(&mce_records) - 1;
	if (records >= MAX_NR_RECORD)
		return;

	if (task) {
		mcestat[records].pid = task->pid;
		strscpy(mcestat[records].comm, task->comm, sizeof(mcestat[records].comm));
	} else {
		mcestat[records].pid = -1;
		strscpy(mcestat[records].comm, "kernel", sizeof(mcestat[records].comm));
	}
	mcestat[records].addr = addr;
	mcestat[records].signal = signal;
	mcestat[records].cmci = cmci;
	mcestat[records].time = ktime_get_ns();
}

static bool is_hugepage(unsigned long pfn)
{
	struct page *head;
	struct page *page = pfn_to_online_page(pfn);

	if (!page)
		return false;

	head = compound_head(page);
	return PageTransHuge(head);
}

static int mcestat_proc_show(struct seq_file *m, void *v)
{
	int records = atomic_read(&mce_records);
	int i;

	seq_puts(m, "INDEX      PID         COMMAND             ADDR HUGE SIGNUM         TIME INTERRUPT\n");
	records = records < MAX_NR_RECORD ? records : MAX_NR_RECORD;
	for (i = 0; i < records; i++) {
		u64 ts = mcestat[i].time;
		unsigned long rem_nsec = do_div(ts, 1000000000);

		seq_printf(m, "%5d %8d%16s %16lx    %1d  %5d %5lu.%06lu %s\n",
			   i, mcestat[i].pid, mcestat[i].comm,
			   mcestat[i].addr,
			   (int)is_hugepage((mcestat[i].addr) >> PAGE_SHIFT),
			   mcestat[i].signal,
			   (unsigned long)ts, rem_nsec / 1000,
			   mcestat[i].cmci ? "CMCI" : "MachineCheck");
	}
	return 0;
}

static ssize_t
mcestat_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	return len;
}

static int mcestat_open(struct inode *inode, struct file *file)
{
	/* If this file was open for write, then erase contents */
	if ((file->f_mode & FMODE_WRITE) && (file->f_flags & O_TRUNC)) {
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		mcestat_reset();
	}

	return single_open_size(file, mcestat_proc_show, NULL, 128 * (1 + MAX_NR_RECORD));
}

static const struct proc_ops mcestat_fops = {
	.proc_open	= mcestat_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_write	= mcestat_write,
	.proc_release	= single_release,
};

static int __init proc_mce_init(void)
{
	if (!proc_create("mcestat", 0644, NULL, &mcestat_fops)) {
		pr_warn("Failed to register /proc/mcestat");
		return -ENOMEM;
	}
	if (!proc_create("mcestat_enabled", 0644, NULL,
			 &mcestat_enabled_fops)) {
		pr_warn("Failed to register /proc/mcestat_enabled");
		goto remove_mcestat;
	}
	return 0;
remove_mcestat:
	remove_proc_entry("mcestat", NULL);
	return -ENOMEM;
}
late_initcall(proc_mce_init);
