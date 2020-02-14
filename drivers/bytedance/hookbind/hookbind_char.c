#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/syscalls.h>
#include <linux/slab.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/ctype.h>
#include "hookbind.h"

MODULE_LICENSE("GPL");
MODULE_VERSION("2.1");

#define DEVICE_NAME	"hbindev"
#define CLASS_NAME	"hbd"
#define MAXLEN		4096
static struct class *ebbcharClass;
static struct device *ebbcharDevice;
static int Major;

static int device_open(struct inode *inodep, struct file *filep)
{
	if(!try_module_get(THIS_MODULE))
		return -EACCES;
	return 0;
}

static ssize_t device_read(struct file *filep, char __user *buffer, 
		size_t len, loff_t *offset)
{
	return dump_dmesg();
}
static ssize_t device_write(struct file *filp, const char __user *buffer, 
		size_t len, loff_t *offset)
{
	return add_new_rule(buffer, len);
}
static int device_release(struct inode *inodep, struct file *filep)
{
	module_put(THIS_MODULE);
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
	int ret;
	ret = register_chrdev(0, DEVICE_NAME, &fops);
	if (ret < 0 ) 
		goto register_chrdev;
	Major = ret;

	ebbcharClass = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(ebbcharClass)) {               
		ret = PTR_ERR(ebbcharClass);
		goto class_create;
	}

	ebbcharDevice = device_create(ebbcharClass, NULL, MKDEV(Major, 0), NULL, DEVICE_NAME);
	if (IS_ERR(ebbcharDevice)) {        
		ret = PTR_ERR(ebbcharDevice);
		goto device_create;
	}
	if(register_hookbind())
		return 0;
	else
		ret = -1;
	device_destroy(ebbcharClass, MKDEV(Major, 0));
device_create:
	class_destroy(ebbcharClass);
class_create:
	unregister_chrdev(Major, DEVICE_NAME);
register_chrdev:
	return ret;
}
/*this function is called when the module is
  *unloaded*/
static void __exit cleanup_my_module(void)
{
	device_destroy(ebbcharClass, MKDEV(Major, 0));     // remove the device
	class_destroy(ebbcharClass);                       // remove the device class
	unregister_chrdev(Major, DEVICE_NAME);
	unregister_hookbind();
}
module_init(init_my_module);
module_exit(cleanup_my_module);
