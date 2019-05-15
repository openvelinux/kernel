/*
 * Copyright (C) 2018-2019 zhenwei pi (pizhenwei@bytedance.com)
 */
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/statfs.h>
#include "blackholefs.h"

struct blackholefs_mount_opts {
	umode_t mode;
};

struct blackholefs_fs_info {
	struct blackholefs_mount_opts mount_opts;
};

#define BHFS_DEFAULT_MODE	0755

static const struct super_operations blackholefs_ops;
static const struct inode_operations blackholefs_dir_inode_operations;

struct inode *blackholefs_get_inode(struct user_namespace *mnt_userns,
		struct super_block *sb, const struct inode *dir, umode_t mode,
		dev_t dev)
{
	struct inode * inode = new_inode(sb);

	if (inode) {
		inode->i_ino = get_next_ino();
		inode_init_owner(mnt_userns, inode, dir, mode);
		inode->i_mapping->a_ops = &ram_aops;
		mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
		mapping_set_unevictable(inode->i_mapping);
		inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

		switch (mode & S_IFMT) {
			default:
				init_special_inode(inode, mode, dev);
				break;

			case S_IFREG:
				inode->i_op = &blackholefs_file_inode_operations;
				inode->i_fop = &blackholefs_file_operations;
				break;

			case S_IFDIR:
				inode->i_op = &blackholefs_dir_inode_operations;
				inode->i_fop = &simple_dir_operations;

				inc_nlink(inode);
				break;

			case S_IFLNK:
				inode->i_op = &page_symlink_inode_operations;
				inode_nohighmem(inode);
				break;
		}
	}

	return inode;
}

static int blackholefs_mknod(struct user_namespace *mnt_userns,
		struct inode *dir, struct dentry *dentry, umode_t mode,
		dev_t dev)
{
	struct inode * inode = blackholefs_get_inode(mnt_userns, dir->i_sb, dir,
						     mode, dev);
	int error = -ENOSPC;

	if (inode) {
		d_instantiate(dentry, inode);
		dget(dentry);
		error = 0;
		dir->i_mtime = dir->i_ctime = current_time(dir);
	}
	return error;
}

static int blackholefs_mkdir(struct user_namespace *mnt_userns,
		struct inode * dir, struct dentry * dentry, umode_t mode)
{
	int retval = blackholefs_mknod(mnt_userns, dir, dentry, mode | S_IFDIR,
				       0);
	if (!retval)
		inc_nlink(dir);

	return retval;
}

static int blackholefs_create(struct user_namespace *mnt_userns,
		struct inode *dir, struct dentry *dentry, umode_t mode,
		bool excl)
{
	return blackholefs_mknod(mnt_userns, dir, dentry, mode | S_IFREG, 0);
}

static int blackholefs_symlink(struct user_namespace *mnt_userns,
		struct inode * dir, struct dentry *dentry,
		const char * symname)
{
	struct inode *inode;
	int error = -ENOSPC;

	inode = blackholefs_get_inode(mnt_userns, dir->i_sb, dir,
				      S_IFLNK | S_IRWXUGO, 0);
	if (inode) {
		int l = strlen(symname)+1;

		error = page_symlink(inode, symname, l);
		if (!error) {
			d_instantiate(dentry, inode);
			dget(dentry);
			dir->i_mtime = dir->i_ctime = current_time(dir);
		} else
			iput(inode);
	}

	return error;
}

static const struct inode_operations blackholefs_dir_inode_operations = {
	.create		= blackholefs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= blackholefs_symlink,
	.mkdir		= blackholefs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= blackholefs_mknod,
	.rename		= simple_rename,
};

/*
 * Display the mount options in /proc/mounts.
 */
static int blackholefs_show_options(struct seq_file *m, struct dentry *root)
{
	struct blackholefs_fs_info *fsi = root->d_sb->s_fs_info;

	if (fsi->mount_opts.mode != BHFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", fsi->mount_opts.mode);

	return 0;
}

static int blackholefs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	TRACE();
	buf->f_type = 0x484f4c45;
	buf->f_bsize = 512;
	buf->f_blocks = 84UL * 1024UL * 1024UL * 1024UL * 1024UL;
	buf->f_bfree = buf->f_blocks;
	buf->f_bavail = buf->f_blocks;
	buf->f_files = 1;
	buf->f_ffree = buf->f_bfree - 1;

	return 0;
}

static const struct super_operations blackholefs_ops = {
	//.statfs	= simple_statfs,
	.statfs		= blackholefs_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= blackholefs_show_options,
};

enum {
	Opt_mode,
	Opt_err
};

static const match_table_t tokens = {
	{Opt_mode, "mode=%o"},
	{Opt_err, NULL}
};

static int blackholefs_parse_options(char *data,
			struct blackholefs_mount_opts *opts)
{
	substring_t args[MAX_OPT_ARGS];
	int option;
	int token;
	char *p;

	opts->mode = BHFS_DEFAULT_MODE;

	while ((p = strsep(&data, ",")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
			case Opt_mode:
				if (match_octal(&args[0], &option))
					return -EINVAL;
				opts->mode = option & S_IALLUGO;
				break;
		}
	}

	return 0;
}

int blackholefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct blackholefs_fs_info *fsi;
	struct inode *inode;
	int err;

	fsi = kzalloc(sizeof(struct blackholefs_fs_info), GFP_KERNEL);
	sb->s_fs_info = fsi;
	if (!fsi)
		return -ENOMEM;

	err = blackholefs_parse_options(data, &fsi->mount_opts);
	if (err)
		return err;

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_magic		= 0x484f4c45;	/* asc "HOLE" */
	sb->s_op		= &blackholefs_ops;
	sb->s_time_gran		= 1;

	inode = blackholefs_get_inode(&init_user_ns, sb, NULL,
				      S_IFDIR | fsi->mount_opts.mode, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

struct dentry *blackholefs_mount(struct file_system_type *fs_type,
		int flags, const char *dev_name, void *data)
{
	TRACE();
	if (!try_module_get(THIS_MODULE)) {
		ERROR();
		return NULL;
	}

	return mount_nodev(fs_type, flags, data, blackholefs_fill_super);
}

static void blackholefs_kill_sb(struct super_block *sb)
{
	TRACE();
	kfree(sb->s_fs_info);
	kill_litter_super(sb);

	module_put(THIS_MODULE);
}

static struct file_system_type blackholefs_fs_type = {
	.name		= "blackholefs",
	.mount		= blackholefs_mount,
	.kill_sb	= blackholefs_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};

static unsigned long once;
static int __init init_blackholefs(void)
{
	if (test_and_set_bit(0, &once))
		return 0;

	TRACE();
	return register_filesystem(&blackholefs_fs_type);
}

static void __exit exit_blackholefs(void)
{
	TRACE();
	unregister_filesystem(&blackholefs_fs_type);
	test_and_clear_bit(0, &once);
}

module_init(init_blackholefs);
module_exit(exit_blackholefs);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhenwei pi pizhewnei@bytedance.com");
