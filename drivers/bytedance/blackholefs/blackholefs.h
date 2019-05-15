/*
 * Copyright (C) 2018-2019 zhenwei pi (pizhenwei@bytedance.com)
 */
#ifndef _LINUX_BLACKHOLEFS_H
#define _LINUX_BLACKHOLEFS_H

/* super operation */
struct inode *blackholefs_get_inode(struct user_namespace *mnt_userns,
				    struct super_block *sb,
				    const struct inode *dir, umode_t mode,
				    dev_t dev);
int blackholefs_fill_super(struct super_block *sb, void *data, int silent);
extern struct dentry *blackholefs_mount(struct file_system_type *fs_type,
	 int flags, const char *dev_name, void *data);

/* file operation */
extern const struct file_operations blackholefs_file_operations;
loff_t blackholefs_llseek(struct file *f, loff_t off, int whence);
ssize_t blackholefs_read(struct file *f, char __user *ubuf, size_t s, loff_t *off);
ssize_t blackholefs_write(struct file *f, const char __user *ubuf, size_t s, loff_t *off);
ssize_t blackholefs_read_iter(struct kiocb *iocb, struct iov_iter *iov);
ssize_t blackholefs_write_iter(struct kiocb *iocb, struct iov_iter *iov);
int blackholefs_fsync(struct file *f, loff_t start, loff_t end, int datasync);
int blackholefs_lock(struct file *f, int operation, struct file_lock *fl);
int blackholefs_flock(struct file *f, int operation, struct file_lock *fl);
ssize_t blackholefs_splice_write(struct pipe_inode_info *p, struct file *f, loff_t *off, size_t len, unsigned int flag);
ssize_t blackholefs_splice_read(struct file *f, loff_t *off, struct pipe_inode_info *p, size_t len, unsigned int flag);
long blackholefs_fallocate(struct file *file, int mode, loff_t offset, loff_t len);

/* inode operation */
extern const struct inode_operations blackholefs_file_inode_operations;

#define ERROR() pr_info("[blackholefs][ERROR]%s, %d\n",__func__,__LINE__)

/*#define DEBUGMSG*/

#ifdef DEBUGMSG
#define TRACE() pr_info("[blackholefs][TRACE]%s, %d\n",__func__,__LINE__)
#define DEBUG(format, ...) pr_info("[blackholefs][DEBUG]" format, __VA_ARGS__)
#else
#define TRACE()
#define DEBUG(format, ...)
#endif

#endif
