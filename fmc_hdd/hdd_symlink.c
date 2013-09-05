/*
 * linux/fs/fmc/hdd_symlink.c
 *
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * Beijing University of Posts and Telecommunications,
 * and CPU Center @ Tsinghua University.
 *
 * from linux/fs/ext2/symlink.c
 *
 * This program can be redistributed under the terms of the GNU Public License.
 */

#include <linux/namei.h>
#include "hdd.h"

/* 快链, 直接从地址数组中取出目的路径 */
static void *hdd_follow_fast_link(struct dentry *dentry, struct nameidata *nd)
{
	struct hdd_inode_info *hi = HDD_I(dentry->d_inode);
	nd_set_link(nd, (char *)hi->i_data);
	return NULL;
}

const struct inode_operations hdd_symlink_inode_operations = {
	.readlink	= generic_readlink,
	.follow_link	= page_follow_link_light,
	.put_link	= page_put_link,
};

const struct inode_operations hdd_fast_symlink_inode_operations = {
	.readlink	= generic_readlink,
	.follow_link	= hdd_follow_fast_link,
};