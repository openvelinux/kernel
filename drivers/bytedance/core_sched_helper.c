// SPDX-License-Identifier: GPL-2.0
#include <linux/sched/task.h>
#include <linux/miscdevice.h>
#include <linux/kallsyms.h>
#include <linux/kthread.h>

/* Define magic number */
#define CORE_SCHED_IOC_MAGIC	'6'

/* Define command */
#define CORE_SCHED_IOC_CREATE	_IOR(CORE_SCHED_IOC_MAGIC, 0, pid_t)
#define CORE_SCHED_IOC_EXIT	_IOW(CORE_SCHED_IOC_MAGIC, 1, pid_t)
#define CORE_SCHED_IOC_MAX	2

static int core_sched_kthread_func(void *unused)
{
	while (!kthread_should_stop())
		schedule_timeout_interruptible(MAX_SCHEDULE_TIMEOUT);

	return 0;
}

static int core_sched_kthread_create(pid_t *pid)
{
	static atomic_t name_cnt = ATOMIC_INIT(0);
	struct task_struct *new_task;

	new_task = kthread_run(core_sched_kthread_func, NULL, "cs-khelper/%d",
				atomic_inc_return(&name_cnt));
	if (IS_ERR(new_task))
		return -ECHILD;

	*pid = task_pid_nr(new_task);
	__module_get(THIS_MODULE);

	return 0;
}

static int core_sched_kthread_exit(pid_t pid)
{
	struct task_struct *task;
	int ret = 0;

	rcu_read_lock();
	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (!task) {
		rcu_read_unlock();
		return -ESRCH;
	}

	get_task_struct(task);
	rcu_read_unlock();
	if (strncmp(task->comm, "cs-khelper/", sizeof("cs-khelper/") - 1))
		ret = -EINVAL;
	if (!ret) {
		kthread_stop(task);
		module_put(THIS_MODULE);
	}
	put_task_struct(task);

	return ret;
}

static long core_sched_ioctl(struct file *file, unsigned int cmd,
				unsigned long param)
{
	pid_t pid;
	int ret;

	if (_IOC_TYPE(cmd) != CORE_SCHED_IOC_MAGIC)
		return -EINVAL;
	if (_IOC_NR(cmd) > CORE_SCHED_IOC_MAX)
		return -EINVAL;

	switch (cmd) {
	case CORE_SCHED_IOC_EXIT:
		ret = get_user(pid, (pid_t __user *)param);
		if (!ret)
			ret = core_sched_kthread_exit(pid);
		break;
	case CORE_SCHED_IOC_CREATE:
		ret = core_sched_kthread_create(&pid);
		if (!ret)
			ret = put_user(pid, (pid_t __user *)param);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static const struct file_operations core_sched_dev_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = core_sched_ioctl,
};

static struct miscdevice core_sched_misc = {
	.name = "core_sched_dev",
	.minor = MISC_DYNAMIC_MINOR,
	.fops = &core_sched_dev_ops
};

module_misc_device(core_sched_misc);
MODULE_LICENSE("GPL v2");
