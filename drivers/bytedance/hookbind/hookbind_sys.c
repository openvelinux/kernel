#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/syscalls.h>
#include <linux/slab.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/ctype.h>
#include <linux/signal.h>

static void **msys_call_table;
static int make_rw(unsigned long address);
static int make_ro(unsigned long address);

struct mapping_rule {
	int	port;		// mapping from
	int	mport;		// mapping to
	struct	pid *pgrp;
	pid_t	nr;
	bool	sig_sent;
	struct	list_head list;
};

static LIST_HEAD(rules);
static DEFINE_SPINLOCK(lock);	// protecting list above

static asmlinkage int (*original_sys_bind)(struct pt_regs *regs);
/* Called _without_ lock on */
static void add(int port, int mport, struct task_struct *tsk)
{
	struct mapping_rule *t = kmalloc(sizeof(*t), GFP_KERNEL);
	if (!t) {
		pr_info("hookbind: *add*: failed to malloc mptable\n");
		return;
	}

	t->port = port;
	t->mport = mport;
	t->sig_sent = false;

	rcu_read_lock();
	t->pgrp = task_pgrp(tsk);
	get_pid(t->pgrp);
	t->nr	= pid_nr(t->pgrp);
	rcu_read_unlock();

	spin_lock(&lock);
	list_add_tail(&t->list, &rules);
	spin_unlock(&lock);

	pr_info("hookbind: *add rule*: pgrp: %d, mapping port %d --> %d\n",
		t->nr, t->port, t->mport);
}
/* Called with lock on */
static void delete(struct mapping_rule *t)
{
	if (t) {
		pr_info("hookbind: *del rule*: pgrp: %d, mapping port %d --> %d\n",
				t->nr, t->port, t->mport);
		put_pid(t->pgrp);
		list_del(&t->list);
		kfree(t);
	}
}
static bool rule_valid(struct mapping_rule *t)
{
	struct task_struct *p;
	if (t->nr != pid_nr(t->pgrp))
		return false;
	do_each_pid_task(t->pgrp, PIDTYPE_PGID, p) {
		if (p)
			return true;
	} while_each_pid_task(t->pgrp, PIDTYPE_PGID, p);
	return false;
}
static int my_move_addr_to_kernel(void __user *uaddr, int ulen, struct sockaddr_storage *kaddr)
{
	if (ulen < 0 || ulen > sizeof(struct sockaddr_storage))
		return -EINVAL;
	if (ulen == 0)
		return 0;
	if (copy_from_user(kaddr, uaddr, ulen))
		return -EFAULT;
	return 0;
}
static asmlinkage int hook_bind_function(struct pt_regs *regs)
{
	int err, port, mport;
	bool hit = false;
	struct pid *group;
	int fd;
	struct sockaddr __user *uaddr;
	int len;
	struct mapping_rule *t, *tmp;
	struct sockaddr *kaddr;
	struct sockaddr_in *addr;
	struct sockaddr_in6 *addr6;
	struct sockaddr_storage address;
	fd = (int)regs->di;
	uaddr = (struct sockaddr *)regs->si;
	len = (int)regs->dx;

	if (!try_module_get(THIS_MODULE))
		goto out;
	if (list_empty(&rules) )
		goto end;
	err = my_move_addr_to_kernel(uaddr, len, &address);
	if (err < 0)
		goto end;
	kaddr = (struct sockaddr *)&address;
	if (kaddr->sa_family != AF_INET && kaddr->sa_family != AF_INET6)
        	goto end;
	addr = (struct sockaddr_in *)kaddr;
	addr6 = (struct sockaddr_in6 *)kaddr;
	group = task_pgrp(current);

	spin_lock(&lock);
	list_for_each_entry_safe(t, tmp, &rules, list) {
		if (!rule_valid(t)) {
			delete(t);
			continue;
		}
		if (t->pgrp != group)
			continue;
		port = t->port;
		mport = t->mport;
		if (kaddr->sa_family == AF_INET &&
				addr->sin_port == htons(t->port)) {
			addr->sin_port = htons(t->mport);
			hit = true;
			break;
		} else if (kaddr->sa_family == AF_INET6 &&
				addr6->sin6_port == htons(t->port)) {
			addr6->sin6_port = htons(t->mport);
			hit = true;
			break;
		}
	}
	spin_unlock(&lock);

	if (hit) {
		if (copy_to_user(uaddr, kaddr, sizeof(struct sockaddr))) {
			pr_info("hookbind: *hit*: copy_to_user failed!\n");
			goto end;
		}
		pr_info("hookbind: *hit*: pid: %d, pgrp: %d, mapping port %d --> %d\n",
				task_pid_nr(current), t->nr, t->port, t->mport);
	}
end:
	module_put(THIS_MODULE);
out:
	return original_sys_bind(regs);
}
/*
Make the memory page writable
This is little risky as directly arch level protection bit is changed
*/
static int make_rw(unsigned long address)
{
	unsigned int level;
	pte_t *pte = lookup_address(address, &level);
	if (pte->pte &~ _PAGE_RW)
		pte->pte |= _PAGE_RW;
	return 0;
}
/* Make the page write protected */
static int make_ro(unsigned long address)
{
	unsigned int level;
	pte_t *pte = lookup_address(address, &level);
	pte->pte = pte->pte &~ _PAGE_RW;
	return 0;
}

static size_t decode(char *argenv,size_t length)
{
	int iport = 0, import = 0, flag = 0;
	ssize_t i = 0;
	while (i < length) {
		if (isspace(argenv[i]) || argenv[i] == '\0') { 
			i++;
		}else if (isdigit(argenv[i])) {
			if (flag == 0)
				iport = iport*10 + (argenv[i] - '0');
			else 
				import = import*10 + (argenv[i] - '0');
			i++;
		}else if (argenv[i] == ':'){
			flag = 1;
			i++;
		} else if (argenv[i] == '/'){
			if (iport > 0xFFFF || import > 0xFFFF ||
				iport <= 0 || import <= 0) {
				pr_info("hookbind: *add rule*: WRONG port %d --> %d\n",
					iport, import);
				i++;
				continue;
			}
			add(iport, import, current);
			flag = 0;
			iport =0;
			import = 0;
			i++;
		} else{
			pr_info("hookbind: str %s\n",argenv);
			pr_info("hookbind: bad str %s\n",&argenv[i]);
			break;
		}
	}
	if (flag == 1 && iport <= 0xFFFF && iport > 0 &&
			import <= 0xFFFF && import > 0)
		add(iport, import, current);
	return i;
}

ssize_t dump_dmesg(void)
{
	struct mapping_rule *t, *tmp;
	struct task_struct *p;
	bool valid;

	spin_lock(&lock);
	list_for_each_entry_safe(t, tmp, &rules, list) {
		valid = false;
		if (t->nr == pid_nr(t->pgrp)) {
			do_each_pid_task(t->pgrp, PIDTYPE_PGID, p) {
				if (p) {
					pr_info("hookbind: *read task*: pid: %d, mapping port %d --> %d\n",
						task_pid_nr(p), t->port, t->mport);
					valid = true;
					/* no break here */
				}
			} while_each_pid_task(t->pgrp, PIDTYPE_PGID, p);
		}
		if (!valid)
			delete(t);
	}
	spin_unlock(&lock);

	return 0;
}

ssize_t add_new_rule(const char __user *buffer,size_t length)
{
	ssize_t res = 0;
	char *bmp = kmalloc((length + 1) * sizeof(char), GFP_KERNEL);
	if (!bmp) {
		pr_info("hookbind: *write*: kmalloc fail\n");
		res = -ENOMEM;
		goto out;
	}
	res = copy_from_user(bmp, buffer, length);
	if (res != 0) {
		res = -EFAULT;
		goto free_bmp;
	}
	bmp[length]='\0';
	res = decode(bmp,length+1);
free_bmp:
	kfree(bmp);
out:
	return res;
}

bool __init register_hookbind(void) {
	msys_call_table = (void *) kallsyms_lookup_name("sys_call_table");
	if (msys_call_table == NULL)
		return false;
	original_sys_bind=msys_call_table[__NR_bind];
	make_rw((unsigned long)msys_call_table);
	rcu_assign_pointer(msys_call_table[__NR_bind],hook_bind_function);
	make_ro((unsigned long)msys_call_table);
	return true;
}

void __exit unregister_hookbind(void)
{
	struct mapping_rule *t, *tmp;
	make_rw((unsigned long)msys_call_table);
	RCU_INIT_POINTER(msys_call_table[__NR_bind],original_sys_bind);
	make_ro((unsigned long)msys_call_table);

	spin_lock(&lock);
	list_for_each_entry_safe(t, tmp, &rules, list) {
		delete(t);
	}
	spin_unlock(&lock);
	synchronize_rcu();

}
