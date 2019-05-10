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

MODULE_LICENSE("GPL");
MODULE_VERSION("2.1");
#define DEVICE_NAME	"hbindev"
#define CLASS_NAME	"hbd"
#define MAXLEN		4096
static void **msys_call_table;
static int make_rw(unsigned long address);
static int make_ro(unsigned long address);
static struct class *ebbcharClass;
static struct device *ebbcharDevice;
static int Major;
static bool unloading;
/* The mapping is valid only if at least one proc in the proc group is alive */
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
static asmlinkage int (*original_sys_bind)(int, struct sockaddr __user *, int);
/* Called _without_ lock on */
static void add(int port, int mport, struct task_struct *tsk)
{
	bool added = false;
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
	t->nr	= pid_nr(t->pgrp);
	rcu_read_unlock();
	spin_lock(&lock);
	if (!unloading) {
		list_add_tail(&t->list, &rules);
		added = true;
	}
	spin_unlock(&lock);
	if (added)
		pr_info("hookbind: *add rule*: pgrp: %d, mapping port %d --> %d\n",
				t->nr, t->port, t->mport);
}
/* Called with lock on */
static void delete(struct mapping_rule *t)
{
	if (t) {
		pr_info("hookbind: *del rule*: pgrp: %d, mapping port %d --> %d\n",
				t->nr, t->port, t->mport);
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
static asmlinkage int hook_bind_function(int fd, struct sockaddr __user *uaddr, int len)
{
	int err, port, mport;
	bool hit = false;
	struct pid *group;
	struct mapping_rule *t, *tmp;
	struct sockaddr *kaddr;
	struct sockaddr_in *addr;
	struct sockaddr_in6 *addr6;
	struct sockaddr_storage address;
	if (list_empty(&rules) || unloading)
		goto end;
	err = my_move_addr_to_kernel(uaddr, len, &address);
	if (err < 0)
		goto end;
	kaddr = (struct sockaddr *)&address;
	if (kaddr->sa_family != AF_INET && kaddr->sa_family != AF_INET6)
        	goto end;
	addr = (struct sockaddr_in *)kaddr;
	addr6 = (struct sockaddr_in6 *)kaddr;
	spin_lock(&lock);
	if (unloading)
		goto out;
	rcu_read_lock();
	group = task_pgrp(current);
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
	rcu_read_unlock();
out:
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
	return original_sys_bind(fd, uaddr, len);
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
static char *trim(char *str)
{
    char *ibuf = str, *obuf = str;
    int i = 0;
    if (str)
    {
        while (*ibuf)
        {
            if (isspace(*ibuf))
                ibuf++;
            else
                obuf[i++] = *ibuf++;
        }
        obuf[i] = '\0';
    }
    return str;
}
static void decode(char *argenv)
{
    char *port, *mport, *tmp = NULL;
    int iport, import;
    if (argenv == NULL) {
        pr_info("hookbind: failed to get argenv\n");
        return ;
    }
    
    argenv = trim(argenv);
    
    while (argenv[0] != '\0' ) {
        port = argenv;
        //pr_info("hookbind debug decode 1\n");
        mport = strchr(argenv, ':');
        if (mport == NULL) {
           pr_info("hookbind: *decode* error, data decode fail, no ':' chart\n");
           return;
        }
        //pr_info("hookbind debug decode 2\n");
        tmp = strchr(argenv, '/');
	if ( tmp != NULL) {
            *tmp = '\0';
        }
        //pr_info("hookbind debug decode 3\n");
	if (mport != NULL) {
            *mport = '\0';
            mport ++;
        }
        //pr_info("hookbind debug decode 4\n");
        if (tmp != NULL)
            tmp ++;
        if (kstrtoint(port, 10, &iport)) {
            pr_info("hookbind: kstrtoint fail port\n");
            return ;
        }
        if (kstrtoint(mport, 10, &import)) {
            pr_info("hookbind: kstrtoint fail mport\n");
            return ;
        }
	if (iport > 0xFFFF || import > 0xFFFF ||
		iport < 0 || import < 0) {
		pr_info("hookbind: *add rule*: WRONG port %d --> %d\n",
			iport, import);
		/* continue with wrong ports converted */
	}
        add(iport, import, current);
        if (tmp)
            argenv = tmp;
        else
            break;
    }
}
static int device_open(struct inode *inodep, struct file *filep)
{
	if (unloading)		/* no more new process allowed */
		return -EACCES;
	return 0;
}
static ssize_t device_read(struct file *filep, char __user *buffer, size_t len, loff_t *offset)
{
	struct mapping_rule *t, *tmp;
	struct task_struct *p;
	bool valid;
	spin_lock(&lock);
	if (unloading)
		goto out;
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
out:
	spin_unlock(&lock);
	return 0;
}
static ssize_t device_write(struct file *filp,
		const char __user *buffer,	/* The buffer to fill with data */
		size_t length,			/* The length of the buffer     */
		loff_t *offset)			/* Our offset in the file       */
{
	int res = 0;
	char *bmp = kmalloc(MAXLEN * sizeof(char), GFP_KERNEL);
	if (!bmp) {
		pr_info("hookbind: *write*: kmalloc fail\n");
		return 0;
	}
        res = copy_from_user(bmp, buffer, MAXLEN);
        //        return -EFAULT; // FIXME
	decode(bmp);
	kfree(bmp);
	
	return (MAXLEN - res);
}
static int device_release(struct inode *inodep, struct file *filep)
{
	return 0;
}
static struct file_operations fops = {
	.read = device_read,
	.write = device_write,
	.open = device_open,
	.release = device_release,
};
/*this function is called when the module is
*loaded (initialization)*/
static int __init init_my_module(void) {
    //misc_deregister(&hbind);
#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,4,0)
    pr_info("can not insmod this module when kernel version < 4.4 \n");
    return -1;
#endif
    Major = register_chrdev(0, DEVICE_NAME, &fops);
    if (Major < 0 ) {
        pr_info("hookbind: *init*: Register character device failed with %d\n", Major);
        return Major;
    }
    // Register the device class
    ebbcharClass = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(ebbcharClass)){                // Check for error and clean up if there is
       unregister_chrdev(Major, DEVICE_NAME);
       pr_alert("hookbind: *init*: failed to register device class\n");
       return PTR_ERR(ebbcharClass);          // Correct way to return an error on a pointer
    }
    // Register the device driver
    ebbcharDevice = device_create(ebbcharClass, NULL, MKDEV(Major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(ebbcharDevice)){               // Clean up if there is an error
       class_destroy(ebbcharClass);           // Repeated code but the alternative is goto statements
       unregister_chrdev(Major, DEVICE_NAME);
       pr_alert("hookbind: *init*: failed to create the device\n");
       return PTR_ERR(ebbcharDevice);
    }
    pr_info("hookbind: *init*\n");
 
    msys_call_table = (void *) kallsyms_lookup_name("sys_call_table");
    original_sys_bind=msys_call_table[__NR_bind];
    make_rw((unsigned long)msys_call_table);
    msys_call_table[__NR_bind]=hook_bind_function;
    make_ro((unsigned long)msys_call_table);
    return 0;
}
/*this function is called when the module is
  *unloaded*/
static void __exit cleanup_my_module(void)
{
	/*make __NR_exit point to the original
	 *sys_exit when our module
	 *is unloaded*/
	struct mapping_rule *t, *tmp;
	int ret;
	unloading = true;
	
	device_destroy(ebbcharClass, MKDEV(Major, 0));     // remove the device
	class_destroy(ebbcharClass);                       // remove the device class
	unregister_chrdev(Major, DEVICE_NAME);
	make_rw((unsigned long)msys_call_table);
	msys_call_table[__NR_bind]=original_sys_bind;
	make_ro((unsigned long)msys_call_table);
again:
	spin_lock(&lock);
	list_for_each_entry_safe(t, tmp, &rules, list) {
		if (!rule_valid(t))
			delete(t);
		else if (!t->sig_sent) {
			ret = kill_pgrp(t->pgrp, SIGKILL, 1); // TODO error check
			t->sig_sent = true;
		}
	}
	if (!list_empty(&rules)) {
		spin_unlock(&lock);
		schedule();
		goto again;
	}
	spin_unlock(&lock);
	pr_info("hookbind: *unload*\n");
}
module_init(init_my_module);
module_exit(cleanup_my_module);
