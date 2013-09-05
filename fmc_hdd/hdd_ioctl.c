/*
 * linux/fs/fmc/hdd_ioctl.c
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * Beijing University of Posts and Telecommunications,
 * and CPU Center @ Tsinghua University.
 *
 * from linux/fs/ext2/ioctl.c
 *
 * This program can be redistributed under the terms of the GNU Public License.
 */

#include <linux/capability.h>
#include <linux/time.h>
#include <linux/sched.h>
#include <linux/compat.h>
#include <linux/mount.h>
#include <asm/current.h>
#include <asm/uaccess.h>

#include "hdd.h"

/* 用于常规文件的标志 */
#define HDD_REG_FLMASK (~(HDD_DIRSYNC_FL | HDD_TOPDIR_FL))
/* 用于不是目录和常规文件的标志 */
#define HDD_OTHER_FLMASK (HDD_NODUMP_FL | HDD_NOATIME_FL)

/* 去除给定类型 inode 的掩码标志 */
static inline __u32 hdd_mask_flags(umode_t mode, __u32 flags)
{
	if (S_ISDIR(mode))
		return flags;
	else if (S_ISREG(mode))
		return flags & HDD_REG_FLMASK;
	else
		return flags & HDD_OTHER_FLMASK;
}

long hdd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct hdd_inode_info *hi = HDD_I(inode);
	unsigned int flags;
	int ret;

	switch (cmd) {
	case HDD_IOC_GETFLAGS:
		hdd_get_inode_flags(hi);
		flags = hi->i_flags & HDD_FL_USER_VISIBLE;
		return put_user(flags, (int __user *) arg);
	case HDD_IOC_SETFLAGS: {
		unsigned int oldflags;

		ret = mnt_want_write(filp->f_path.mnt);
		if (ret)
			return ret;

		if (!is_owner_or_cap(inode)) {
			ret = -EACCES;
			goto setflags_out;
		}

		if (get_user(flags, (int __user *) arg)) {
			ret = -EFAULT;
			goto setflags_out;
		}

		flags = hdd_mask_flags(inode->i_mode, flags);

		mutex_lock(&inode->i_mutex);

		if (IS_NOQUOTA(inode)) {
			mutex_unlock(&inode->i_mutex);
			ret = -EPERM;
			goto setflags_out;
		}
		oldflags = hi->i_flags;

		if ((flags ^ oldflags) & (HDD_APPEND_FL | HDD_IMMUTABLE_FL)) {
			if (!capable(CAP_LINUX_IMMUTABLE)) {
				mutex_unlock(&inode->i_mutex);
				ret = -EPERM;
				goto setflags_out;
			}
		}

		flags = flags & HDD_FL_USER_MODIFIABLE;
		flags |= oldflags & ~HDD_FL_USER_MODIFIABLE;
		hi->i_flags = flags;
		mutex_unlock(&inode->i_mutex);

		hdd_set_inode_flags(inode);
		inode->i_ctime = CURRENT_TIME_SEC;
		mark_inode_dirty(inode);
setflags_out:
		mnt_drop_write(filp->f_path.mnt);
		return ret;
	}
	default:
		return -ENOTTY;
	}
}
