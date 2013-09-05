/*
 * linux/fs/fmc/ssd_super.c
 *
 * Copyright (C) 2013 Liang Xuesen, <liangxuesen@gmail.com>
 * @ Beijing University of Posts and Telecommunications,
 * @ CPU center of Tsinghua University.
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/proc_fs.h>
#include <linux/buffer_head.h>
#include <linux/backing-dev.h>
#include <linux/kthread.h>
#include <linux/parser.h>
#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/random.h>
#include <linux/exportfs.h>

#include "fmc_fs.h"
#include "fmc_ssd.h"

LIST_HEAD(ssd_sb_infos);			/* 记录所有 sb 私有信息的链表 */
DEFINE_SPINLOCK(ssd_sbi_lock);			/* 保护 sb 链表的自旋锁 */

static struct file_system_type ssd_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "fmc_ssd",
	.get_sb		= ssd_get_sb,		/* 挂载文件系统 */
	.kill_sb	= kill_block_super,	/* 清理超级块 */
	.fs_flags	= FS_REQUIRES_DEV,	/* 标志: 需要设备 */
};

/* 初始化文件系统 */
static int __init init_ssd_fs(void)
{
	int err;

	err = init_inodecache();
	if (err) goto fail;
	err = create_node_manager_caches();
	if (err) goto fail;
	err = create_gc_caches();
	if (err) goto fail;
	err = create_checkpoint_caches();
	if (err) goto fail;

	err = register_filesystem(&ssd_fs_type);/* 注册文件系统 */
	if (err)
		goto fail;
fail:
	return err;
}

/* 卸载文件系统 */
static void __exit exit_ssd_fs(void)
{
	unregister_filesystem(&ssd_fs_type);	/* 注销文件系统 */

	destroy_checkpoint_caches();
	destroy_gc_caches();
	destroy_node_manager_caches();
	destroy_inodecache();
}

module_init(init_ssd_fs)	/* 安装模块 */
module_exit(exit_ssd_fs)	/* 卸载模块 */

MODULE_AUTHOR("Xuesen Liang");
MODULE_DESCRIPTION("FLASH MODULE OF FMCFS");
MODULE_LICENSE("GPL");