/*
 * linux/fs/fmc/hdd_file.c
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * Beijing University of Posts and Telecommunications,
 * CPU Center @ Tsinghua University.
 *
 * from linux/fs/ext2/file.c
 *
 * This program can be redistributed under the terms of the GNU Public License.
 */

#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/falloc.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/mount.h>
#include <linux/time.h>

#include "hdd.h"

const struct file_operations hdd_file_operations = {
	.llseek		= generic_file_llseek,

	.read		= do_sync_read,
	.write		= do_sync_write,

	.aio_read	= generic_file_aio_read,
	.aio_write	= generic_file_aio_write,

	.open		= generic_file_open,

	.mmap		= generic_file_mmap,
	.fsync		= simple_fsync,

	.mmap		= f2fs_file_mmap,
	.fsync		= f2fs_sync_file,
	.fallocate	= f2fs_fallocate,

	.unlocked_ioctl = hdd_ioctl,

	.splice_read	= generic_file_splice_read,
	.splice_write	= generic_file_splice_write,
};

/* 读取文件基本属性 */
static int hdd_getattr(struct vfsmount *mnt,
	struct dentry *dentry, struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;

	generic_fillattr(inode, stat);

	stat->blocks <<= 3;

	return 0;
}

const struct inode_operations hdd_file_inode_operations = { /* inode.c */
	.truncate	= hdd_truncate,
	.getattr	= hdd_getattr,
	.fiemap		= hdd_fiemap,
};
