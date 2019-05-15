/*
 * Copyright (C) 2018-2019 zhenwei pi (pizhenwei@bytedance.com)
 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include "blackholefs.h"

static unsigned long blackholefs_get_unmapped_area(struct file *file,
		unsigned long addr, unsigned long len, unsigned long pgoff,
		unsigned long flags)
{
	return current->mm->get_unmapped_area(file, addr, len, pgoff, flags);
}

loff_t blackholefs_llseek(struct file *f, loff_t off, int whence)
{
	TRACE();
	return -ESPIPE;
}

ssize_t blackholefs_read(struct file *f, char __user *ubuf, size_t s,
			loff_t *off)
{
	loff_t read_size;
	char *fname = "UNKNOWN";

	TRACE();
	if (!f->f_inode) {
		ERROR();
		return 0;
	}

	if (f->f_path.dentry && f->f_path.dentry->d_name.name)
		fname = (char*)f->f_path.dentry->d_name.name;

	if (*off >= f->f_inode->i_size)
		return 0;

	DEBUG("[READ][%s] : f->f_pos = %lld, off = %lld, size = %ld, inode->i_size = %lld\n",
			fname, f->f_pos, *off, s, f->f_inode->i_size);

	if ((*off + s ) >= f->f_inode->i_size)
		read_size = f->f_inode->i_size - *off;
	else
		read_size = s;

	*off += read_size;
	if(clear_user(ubuf, s)) {
		ERROR();
	}

	return read_size;
}

ssize_t blackholefs_write(struct file *f, const char __user *ubuf, size_t s,
			loff_t *off)
{
	loff_t write_size;
	char *fname = "UNKNOWN";

	TRACE();
	if (!f->f_inode) {
		ERROR();
		return 0;
	}

	if (f->f_path.dentry && f->f_path.dentry->d_name.name)
		fname = (char*)f->f_path.dentry->d_name.name;

	DEBUG("[WRITE][%s] : f->f_pos = %lld, off = %lld, size = %ld, f->f_inode->i_size = %lld\n",
			fname, f->f_pos, *off, s, f->f_inode->i_size);

	write_size = *off + s;
	if (write_size > f->f_inode->i_size) {
		f->f_inode->i_size = write_size;
		DEBUG("[WRITE]new size : f->f_inode->i_size = %lld\n", f->f_inode->i_size);
	}

	*off = write_size;

	return s;
}

ssize_t blackholefs_read_iter(struct kiocb *iocb, struct iov_iter *iov)
{
	TRACE();

	return -ESPIPE;
}

ssize_t blackholefs_write_iter(struct kiocb *iocb, struct iov_iter *iov)
{
	TRACE();

	return -ESPIPE;
}

int blackholefs_fsync(struct file *f, loff_t start, loff_t end, int datasync)
{
	TRACE();

	return 0;
}

int blackholefs_lock(struct file *f, int operation, struct file_lock *fl)
{
	TRACE();

	return -ESPIPE;
}

int blackholefs_flock(struct file *f, int operation, struct file_lock *fl)
{
	TRACE();

	return -ESPIPE;
}

ssize_t blackholefs_splice_write(struct pipe_inode_info *p, struct file *f, loff_t *off, size_t len, unsigned int flag)
{
	TRACE();

	return -ESPIPE;
}

ssize_t blackholefs_splice_read(struct file *f, loff_t *off, struct pipe_inode_info *p, size_t len, unsigned int flag)
{
	TRACE();

	return -ESPIPE;
}

long blackholefs_fallocate(struct file *f, int mode, loff_t offset, loff_t len)
{
	TRACE();
	if (!f->f_inode) {
		ERROR();
		return 0;
	}

	f->f_inode->i_size = len;

	return len;
}

const struct file_operations blackholefs_file_operations = {
	//.llseek         = blackholefs_llseek,
	.llseek		= generic_file_llseek,
	.read           = blackholefs_read,
	.write          = blackholefs_write,
	//.read_iter	= generic_file_read_iter,
	//.read_iter   	= blackholefs_read_iter,
	//.read_iter	= generic_file_read_iter,
	//.write_iter   = blackholefs_write_iter,
	//.write_iter	= generic_file_write_iter,
	//.mmap	    	= generic_file_mmap,
	.fsync	    	= blackholefs_fsync,
	//.fsync		= noop_fsync,
	.lock           = blackholefs_lock,
	.splice_write	= blackholefs_splice_write,
	//.splice_write	= iter_file_splice_write,
	.splice_read	= blackholefs_splice_read,
	//.splice_read	= generic_file_splice_read,
	.fallocate      = blackholefs_fallocate,
	.get_unmapped_area	= blackholefs_get_unmapped_area,
};

const struct inode_operations blackholefs_file_inode_operations = {
	.setattr	= simple_setattr,
	.getattr	= simple_getattr,
};
