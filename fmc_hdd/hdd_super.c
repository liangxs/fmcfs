/*
 * fmcfs/fmc_hdd/super.c
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * @ Beijing University of Posts and Telecommunications,
 * @ CPU & SoC Center of Tsinghua University.
 *
 * This program can be redistributed under the terms of the GNU Public License.
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/parser.h>
#include <linux/smp_lock.h>
#include <linux/buffer_head.h>
#include <linux/exportfs.h>
#include <linux/vfs.h>
#include <linux/random.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/seq_file.h>
#include <linux/log2.h>
#include <asm/uaccess.h>
#include <linux/statfs.h>
#include <linux/backing-dev.h>
#include <linux/kthread.h>

#include "../fmc_fs.h"
#include "hdd.h"

static struct kmem_cache *hdd_inode_cachep;

static void hdd_release_ssd(struct hdd_sb_info *sbi);
static void init_once(void *foo);

/* 输出信息 */
void hdd_msg(struct super_block *sb, const char *level, 
	     const char *func, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	printk("%sFMC-hdd (device %s): %s: ", level, sb->s_id, func);
	vprintk(fmt, args);
	printk("\n")
	va_end(args);
}

/* 分配 inode 对象及私有信息*/
static struct inode *hdd_alloc_inode(struct super_block *sb)
{
	struct hdd_inode_info *hi;

	hi = kmem_cache_alloc(hdd_inode_cachep, GFP_NOFS | __GFP_ZERO);
	if (!hi)
		return NULL;

	hi->vfs_inode.i_version = 1;

	return &hi->vfs_inode;
}

/* 释放 inode 对象及私有信息*/
static void hdd_destroy_inode(struct inode *inode)
{
	kmem_cache_free(hdd_inode_cachep, HDD_I(inode));
}

/* 释放 inode 对象私有信息中分配的空间 */
static void hdd_clear_inode(struct inode *inode)
{
	// void;
}

static void hdd_do_sync_fs(struct super_block *sb, int wait)
{
	/* 若只读文件系统, 或未修改, 则直接返回;
	   占有超级块总写锁, 禁止别处读写超级块;
	   然后更新相关信息: 空闲块数等;
	   标记超级块缓冲区为脏.
	*/

	struct hdd_sb_info sbi = HDD_SB(sb);
	struct hdd_super_block *hdd_sb = sbi->hdd_sb;
	unsigned int tmp = 0;
	int i = 0;

	tmp = percpu_counter_sum_positive(&sbi->usr_blocks);
	hdd_sb->s_user_blocks = cpu_to_le32(tmp);

	tmp = percpu_counter_sum_positive(&sbi->free_blks_count);
	hdd_sb->s_free_blocks_count = cpu_to_le32(tmp);

	tmp = percpu_counter_sum_positive(&sbi->free_inodes_count);
	hdd_sb->s_free_inodes_count = cpu_to_le32(tmp);

	hdd_sb->s_wtime = cpu_to_le32(get_seconds()); /* when 写回的 */

	tmp = percpu_counter_sum_positive(&sbi->ssd_blks_count);
	hdd_sb->s_ssd_blocks_count = cpu_to_le32(tmp);	/* 到 SSD 的块数 */

	hdd_sb->s_hdd_idx = cpu_to_le32(sbi->hdd_idx);	/* 在 SSD 中的下标 */

	hdd_sb->s_cld_blocks_count = cpu_to_le64(
		percpu_counter_sum_positive(&sbi->cld_blks_count));

	tmp = percpu_counter_sum_positive(&sbi->cld_files_count);
	hdd_sb->s_cld_files_count = cpu_to_le32(tmp);	/* 迁移到 Cloud 的文件数 */

	hdd_sb->s_total_access = cpu_to_le64(
		percpu_counter_sum_positive(&sbi->total_access));/* 总访问次数 */

	for (i = 0; i < FMC_MAX_LEVELS) {		/* 约1K-每个访问级别的块数 */
		tmp = percpu_counter_sum_positive(&sbi->blks_per_lvl[i]);
		hdd_sb->s_blks_per_level[i] = cpu_to_le32(tmp);
	}

	mark_buffer_dirty(sbi->hdd_bh);       /* 标记超级缓冲块为脏 */
	if (wait)
		sync_dirty_buffer(sbi->hdd_bh);       /* 同步缓冲块 */

	sbi->s_dirty = 0;
	sb->s_dirt = 0;
}

/* 同步文件系统 */
static int hdd_sync_fs(struct super_block *sb, int wait)
{
	struct hdd_sb_info sbi = HDD_SB(sb);

	if (sb->s_flags & MS_RDONLY || !sbi->s_dirty)
		return 0;
	
	down_write(&sbi->sbi_rwsem);
	hdd_do_sync_fs(sb, wait);
	up_write(&sbi->sbi_rwsem);

	return 0;
}

/* 写超级块到磁盘上 */
void hdd_write_super(struct super_block *sb)
{
	if (!(sb->s_flags & MS_RDONLY))
		hdd_sync_fs(sb, 1);
	else
		sb->s_dirt = 0;
}

/* 释放超级块私有信息 */
static void hdd_put_super(struct super_block *sb)
{
	struct hdd_sb_info *sbi = HDD_SB(sb);
	int i = 0;

	down_write(&sbi->sbi_rwsem);

	if ((sb->s_flags & MS_RDONLY) && (sb->s_dirt || sbi->s_dirty))
		hdd_do_sync_fs(sb, 1);	/* 写超级块 */

	hdd_release_ssd(sbi);		/* 取消与 ssd 的关联 */

	sb->s_fs_info = NULL;

	percpu_counter_destroy(&sbi->usr_blocks);
	percpu_counter_destroy(&sbi->free_blks_count);
	percpu_counter_destroy(&sbi->free_inodes_count);
	percpu_counter_destroy(&sbi->total_access);
	percpu_counter_destroy(&sbi->ssd_blks_count);
	for (i = 0; i < FMC_MAX_LEVELS; i++)
		percpu_counter_destroy(&sbi->blks_per_lvl[i]);

	for (i = 0; i < sbi->gdt_blocks; i++)
		if (sbi->group_desc[i])
			brelse(sbi->group_desc[i]);

	kfree(sbi->group_desc);
	brelse (sbi->hdd_bh);

	kfree(sbi->blks_per_lvl);
	kfree(sbi->blkgrp_lock);

	up_write(&sbi->sbi_rwsem);

	kfree(sbi);
}

/* 取得文件系统状态 */
static int hdd_statfs (struct dentry * dentry, struct kstatfs * buf)
{
	struct super_block *sb = dentry->d_sb;
	struct hdd_sb_info *sbi = HDD_SB(sb);
	struct hdd_super_block *hdd_sb = sbi->hdd_sb;
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);

	buf->f_type = HDD_MAGIC;
	buf->f_bsize = sbi->block_size;

	buf->f_blocks = sbi->groups_count * sbi->block_size * 8 -
		(sbi->blks_per_group - sbi->last_group_blks);

	buf->f_bfree = percpu_counter_sum_positive(&sbi->free_blks_count);
	buf->f_bavail = buf->f_bfree;

	buf->f_files = sbi->inodes_count;
	buf->f_ffree = percpu_counter_sum_positive(&sbi->free_inodes_count);

	buf->f_namelen = HDD_NAME_LEN;

	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);

	return 0;
}

/* 显示可用选项 */
static int hdd_show_options(struct seq_file *seq, struct vfsmount *vfs)
{
	struct super_block *sb = vfs->mnt_sb;
	struct hdd_sb_info *sbi = HDD_SB(sb);

	if (sbi->sb_block != 1)		/* 超级块所在块号 */
		seq_printf(seq, ",sb=%lu", sbi->sb_block);

	if(sbi->hdd_sb->s_ssd_name)
		seq_puts(seq, "ssd device: %s", sbi->hdd_sb->s_ssd_name);

	if (sbi->ssd_info) 
		seq_puts(seq, "ssd uuid: %8x-%8x-%8x-%8x", 
			sbi->hdd_sb->s_ssd_uuid[0],
			sbi->hdd_sb->s_ssd_uuid[4],
			sbi->hdd_sb->s_ssd_uuid[8],
			sbi->hdd_sb->s_ssd_uuid[12]);
	
	if (sbi->cld_name)
		seq_puts(seq, ", cloud service: %s", sbi->cld_name);

	seq_puts(seq, ", upper ratio: %u%%", sbi->upper_ratio);
	seq_puts(seq, ", cloud service: %u seconds", sbi->max_unaccess);

	return 0;
}

/* 超级块处理函数 */
static struct super_operations hdd_sops = {
	.alloc_inode	= hdd_alloc_inode,	/* 从 cache 中分配 inode 对象和私有信息 */
	.destroy_inode	= hdd_destroy_inode,	/* 从 cache 中释放 inode 空间 */
	.clear_inode	= hdd_clear_inode,
	.sync_fs	= hdd_sync_fs,
	.write_super	= hdd_write_super,
	.put_super	= hdd_put_super,
	.statfs		= hdd_statfs,
	.show_options	= hdd_show_options,
	.write_inode	= hdd_write_inode,/* inode.c */
	.delete_inode	= hdd_delete_inode,/* inode.c */
};

static struct inode *hdd_nfs_get_inode(struct super_block *sb,
	u64 ino, u32 generation)
{
	struct inode *inode;

	if (ino < HDD_FIRST_INO && ino != HDD_ROOT_INO) /* 或为根 inode */
		return ERR_PTR(-ESTALE);
	if (ino >= le32_to_cpu(HDD_SB(sb)->hdd_sb->s_inodes_count))/* 或不超过最大值 */
		return ERR_PTR(-ESTALE);

	inode = hdd_iget(sb, ino);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	if (generation && inode->i_generation != generation) {
		iput(inode);
		return ERR_PTR(-ESTALE);
	}
	return inode;
}

static struct dentry *hdd_fh_to_dentry(struct super_block *sb, 
	struct fid *fid, int fh_len, int fh_type)
{
	return generic_fh_to_dentry(sb, fid, fh_len, fh_type,
				    hdd_nfs_get_inode);
}

static struct dentry *hdd_fh_to_parent(struct super_block *sb, 
	struct fid *fid, int fh_len, int fh_type)
{
	return generic_fh_to_parent(sb, fid, fh_len, fh_type,
				    hdd_nfs_get_inode);
}

static const struct export_operations hdd_export_ops = {
	.fh_to_dentry = hdd_fh_to_dentry,
	.fh_to_parent = hdd_fh_to_parent,
	.get_parent = hdd_get_parent, /* namei.c */
};

/* 初始化私有信息各字段 */
static int init_sb_info(struct hdd_sb_info *sbi)
{
	int err = 0;
	struct hdd_super_block *h = sbi->hdd_sb;
	int i = 0;

	sbi->sb_block = 1;		/* 超级块所在的块号 */
	sbi->mount_state = le32_to_cpu(h->s_state);/* 挂载状态 */

	bgl_lock_init(sbi->blkgrp_lock);

	sbi->groups_count= le32_to_cpu(h->s_groups_count);/* 组总数 */
	sbi->blocks_count= le32_to_cpu(h->s_blocks_count);/* 块总数 */
	sbi->inodes_count= le32_to_cpu(h->s_inodes_count);/* inode 总数 */

	sbi->itable_blocks=le32_to_cpu(h->s_itable_blocks);/* 块组中 inode 的块数 */
	sbi->inode_size  = le32_to_cpu(h->s_inode_size);/* inode 大小 */	
	sbi->block_size  = le32_to_cpu(h->s_block_size);/* 块大小 */
	sbi->log_blocksize=le32_to_cpu(h->s_log_block_size);/* 块大小 log */

	sbi->blks_per_group  = le32_to_cpu(h->s_blocks_per_group);/* 块组中的 block 总数 */
	sbi->last_group_blks = le32_to_cpu(h->s_last_group_blocks);/* 最后块组的总块数 */
	sbi->inodes_per_group= le32_to_cpu(h->s_inodes_per_group);/* 块组中的 inode 数 */
	sbi->inodes_per_block= HDD_BLOCK_SIZE / HDD_INODE_SIZE;/* 块中的 inode 数 */
	sbi->desc_per_block  = HDD_BLOCK_SIZE / sizeof(struct hdd_group_desc);/* 块中的desc数 */
	sbi->desc_per_blk_bits=7;		/* 块中的组描述符 bit - 128个组描述符 */
	sbi->addr_per_blk = HDD_ADDR_PER_BLOCK;	/* 每块中的地址个数 */
	sbi->root_ino_num = HDD_ROOT_INO;	/* 根 inode 号 */
	sbi->first_ino    = HDD_FIRST_INO;	/* 首个非保留 inode 号 */
	sbi->grp_data_offset = 3 + sbi->gdt_blocks + sbi->itable_blocks;

	memcpy(sbi->cld_name, h->s_cld_name, 16);/* 云存储名字 */
	sbi->hdd_idx = le32_to_cpu(h->s_hdd_idx);/* 在 ssd 中 uuid 数组的下标 */
	sbi->cld_blks_count = le64_to_cpu(h->s_cld_blocks_count)/* 迁移到 Cloud 的块数 */
	sbi->cld_files_count= le32_to_cpu(h->s_cld_files_count);/* 迁移到 Cloud 的文件数 */
	sbi->upper_ratio    = le32_to_cpu(h->s_upper_ratio);	/* 触发迁移块的空间使用率 */
	sbi->max_unaccess   = le32_to_cpu(h->s_max_unaccess);	/* 触发迁移文件的未访问年龄 - 秒 */

	init_rwsem(&sbi->sbi_rwsem);
	mutex_init(&sbi->ssd_mutex);
	spin_lock_init(&sbi->cld_lock);

	err = percpu_counter_init(&sbi->usr_blocks, 
		le32_to_cpu(h->s_user_blocks));

	if (!err) {
		err = percpu_counter_init(&sbi->free_blks_count, 
			le32_to_cpu(h->s_free_blocks_count));
	}
	
	if (!err) {
		err = percpu_counter_init(&sbi->free_inodes_count, 
			le32_to_cpu(h->s_free_inodes_count));
	}
	if (!err) {
		err = percpu_counter_init(&sbi->total_access, 
			le32_to_cpu(h->s_total_access));
	}
	for (i = 0; i < FMC_MAX_LEVELS; ++i) {
		err = percpu_counter_init(&sbi->blks_per_lvl[i], 
			le32_to_cpu(h->s_blks_per_level[i]));
		if (err)
			break;		
	}
	if (!err){
		err = percpu_counter_init(&sbi->ssd_blks_count, 
			le32_to_cpu(h->s_ssd_blocks_count));
	}
	
	return err;
}

/* 最大文件长度 */
static loff_t hdd_max_size(void)
{
	loff_t upperlimit = (1LL << 32) - 1;
	loff_t result = 0;
	loff_t direct = HDD_NDIR_BLOCKS;
	loff_t indirect = HDD_ADDR_PER_BLOCK;
	loff_t double_ind = indirect << 10;
	loff_t triple_ind = double_ind << 10;

	result = direct + indirect + double_ind + triple_ind; /* 总块数 */

	result <<= HDD_BLOCK_LOG_SIZE;

	return result; /* 3,430,734,274,560 Bytes = 3.12 TiB */
}

/* 添加到 ssd 中 */
static int set_ssd_info(struct hdd_sb_info *sbi, struct ssd_sb_info *sdi)
{
	int i = 0;
	char * uuid;
	char zeros[16] = {'\0'};

	for (i = 0; i < SSD_MAX_HDDS; ++i) {
		uuid = sdi->sbc->hdd_uuids[i];
		if (memcmp(uuid, zeros, 16) == 0) {
			memcpy(uuid, sbi->hdd_sb->s_uuid, 16);
			sdi->hdd_count ++;
			sdi->sbc_dirty = 1;
			break;
		}
	}
	return i;
}

/* 取得或设置 SSD 信息 */
static int hdd_get_ssd(struct hdd_sb_info *sbi)
{
	char tmp[16] = {'\0'};
	char *s_uuid = sbi->hdd_sb->s_ssd_uuid;
	char *h_uuid = sbi->hdd_sb->s_uuid;
	struct ssd_sb_info *sdi;
	struct list_head *pos;
	int err = -1;

	if (memcmp(tmp, s_uuid, 16) == 0) /* ssd 的 UUID 为空, 即不需 ssd */
		return 0;

	spin_lock(&ssd_sbi_lock);
	/* ssd 的 UUID 非空, 即已有目标 */
	if (sbi->hdd_idx >= 0 && sbi->hdd_idx < SSD_MAX_HDDS) /* 已设置过目标 */
		list_for_each(pos, &ssd_sb_infos) {
			sdi = list_entry(pos, struct ssd_sb_info, sdi_list);
			if (memcmp(sdi->uuid, s_uuid, 16) != 0)
				continue;

			 /* 找到目标 ssd */
			if(memcmp(sdi->sbc->hdd_uuids[sbi->hdd_idx],
				h_uuid, 16) == 0	/* uuid 正确 */
			&& sdi->hdd_info[sbi->hdd_idx] == NULL) {/* 且位置空闲 */
				spin_lock(&sdi->hdd_info_lock);
				sdi->hdd_info[sbi->hdd_idx] = sbi;
				spin_unlock(&sdi->hdd_info_lock);

				atomic_inc(&sdi->refernce);
				
				sbi->ssd_info = sdi;
				sbi->ssd_bdev = sdi->bdev;
				err = 0;
			} else {/* 目标位置的 uuid 不对, 或位置不空闲, 即有错 */
				printk(KERN_ERR "ssd info is not right");
			}
			break;
		}
	else {	/* 想设置目标 ssd */
		if (sbi->sb->s_flags & MS_RDONLY){
			spin_unlock(&ssd_sbi_lock);
			printk(KERN_ERR "Mount read only, cannot set dest ssd\n");
			return -1;
		}
		
		list_for_each(pos, &ssd_sb_infos) {
			sdi = list_entry(pos, struct ssd_sb_info, sdi_list);
			if (memcmp(sdi->uuid, s_uuid, 16) != 0)
				continue;

			 /* 找到目标 ssd */
			spin_lock(&sdi->hdd_count_lock);
			if (sdi->hdd_count != SSD_MAX_HDDS){/* 存在空闲位置 */
				sbi->hdd_idx = set_ssd_info(sbi, sdi);
				sbi->s_dirty = 1;

				spin_lock(&sdi->hdd_info_lock);
				sdi->hdd_info[sbi->hdd_idx] = sbi;
				spin_unlock(&sdi->hdd_info_lock);

				atomic_inc(&sdi->refernce);

				sbi->ssd_info = sdi;
				sbi->ssd_bdev = sdi->bdev;
				err = 0;
			} else 
				printk(KERN_ERR	"No available position for hdd.\n");
			spin_unlock(&sdi->hdd_count_lock);
			break;
		}
	}
	spin_unlock(&ssd_sbi_lock);
	
	return err;
}

/* 释放 ssd 信息 */
static void hdd_release_ssd(struct hdd_sb_info *sbi)
{
	struct ssd_sb_info *ssdi = sbi->ssd_info;

	if (!ssdi)	/* 无 ssd */
		return;

	mutex_lock(&sbi->ssd_mutex);
	sbi->ssd_info = NULL;
	mutex_unlock(&sbi->ssd_mutex);

	spin_lock(&ssdi->hdd_info_lock);
	ssdi->hdd_info[sbi->hdd_idx] = NULL;/* 取消 ssd 对 hdd 的记录 */
	spin_unlock(&ssdi->hdd_info_lock);

	atomic_dec(&ssdi->refernce);	/* 减少 ssd 的引用计数 */

	if(sbi->s_dirty){/* 设置了目标 ssd */
		sbi->hdd_sb->s_hdd_idx = cpu_to_le32(sbi->hdd_idx);
		sbi->hdd_sb->s_mtime = cpu_to_le32(get_seconds());

		mark_buffer_dirty(sbi->hdd_bh);
		sync_dirty_buffer(sbi->hdd_bh);

		sbi->s_dirty = 0;
	}
}

/* 检查文件系统 */
static int hdd_setup_super (struct super_block * sb,
	struct hdd_super_block * hdd_sb, int read_only)
{
	struct hdd_sb_info *sbi = HDD_SB(sb);

	if (read_only)
		return 0;

	/* 检查是否存在错误 */
	if (!(sbi->mount_state & HDD_VALID_FS))
		printk ("FMC_hdd warning: mounting unchecked fs.");
	else if ((sbi->mount_state & HDD_ERROR_FS))
		printk ("FMC_hdd warning: mounting fs with errors, ");

	lock_kernel();
	
	if (hdd_sb->s_state & cpu_to_le32(HDD_VALID_FS))/* 之前被干净卸载 */
		hdd_sb->s_state &= cpu_to_le32(~HDD_VALID_FS);/* 现在标记为已挂载 */

	if(sbi->s_dirty)	/* 标记目标 ssd */
		sbi->hdd_sb->s_hdd_idx = cpu_to_le32(sbi->hdd_idx);

	sbi->hdd_sb->s_mtime = cpu_to_le32(get_seconds());/* 标记挂载时间 */

	mark_buffer_dirty(sbi->hdd_bh);
	sync_dirty_buffer(sbi->hdd_bh);
	
	sbi->s_dirty = 0;

	unlock_kernel();

	return 0;
}

/* 填充超级块对象 */
static int hdd_fill_sb(struct super_block *sb, void *data, int silent)
{
	struct buffer_head *hdd_bh;		/* 缓冲块头 */
	struct hdd_super_block *hdd_sb;		/* 磁盘超级块 */
	struct hdd_sb_info *sbi;		/* 超级块私有信息 */
	struct inode *root;
	long err = -EINVAL;
	int i, j;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);/* 分配私有信息 */
	if (!sbi) {
		hdd_msg(sb, KERN_ERR,__func__,"Unable to allocate sbi");
		return -ENOMEM;
	}

	i = sizeof(struct blockgroup_lock);
	sbi->blkgrp_lock = kzalloc(i, GFP_KERNEL);/* 分配组描述符锁 */
	if (!sbi->blkgrp_lock) {
		err = -ENOMEM;
		hdd_msg(sb, KERN_ERR,__func__,"Unable to allocate blkgrp_lock");
		goto free_sbi;
	}

	i = sizeof(struct percpu_counter) * FMC_MAX_LEVELS;
	sbi->blks_per_lvl = kzalloc(i, GFP_KERNEL);/* 分配统计信息 */
	if (!sbi->blks_per_lvl) {
		err = -ENOMEM;
		hdd_msg(sb, KERN_ERR,__func__,"Unable to allocate blks_per_lvl");
		goto free_grplock;

	}

	if (!sb_set_blocksize(sb, HDD_BLOCK_SIZE)) {/* 设置块大小 */
		hdd_msg(sb, KERN_ERR,__func__,"Unable to set blocksize");
		goto free_blks_level;
	}

	hdd_bh = sb_bread(sb, 1);		/* 读取磁盘超级块 */
	if (!hdd_bh) {
		hdd_msg(sb, KERN_ERR,__func__,"Unable to read superblock");
		goto free_blks_level;
	}
	hdd_sb = (struct hdd_super_block *) ((char *)hdd_bh->b_data);
	sbi->hdd_sb = hdd_sb;

	sb->s_magic = le32_to_cpu(hdd_sb->s_magic);	/* 验证魔数 */
	if (sb->s_magic != HDD_MAGIC) {
		hdd_msg(sb, KERN_ERR,__func__,"not a FMC_HDD device");
		goto release_hdd_bh;
	}

	sbi->gdt_blocks = le32_to_cpu(hdd_sb->s_gdt_blocks)
	i = sbi->gdt_blocks * sizeof (struct buffer_head *);
	sbi->group_desc = kmalloc (i, GFP_KERNEL);/* 分配组描述符块的缓冲头 */
	if (!sbi->group_desc) {
		err = -ENOMEM;
		hdd_msg(sb, KERN_ERR,__func__,"Unable to allocate group_desc");
		goto release_hdd_bh;
	}

	for (i = 0; i < sbi->gdt_blocks; i++) {	/* 读组描述符缓冲块 */
		sbi->group_desc[i] = sb_bread(sb, 2 + i);
		if (!sbi->group_desc[i]) {
			for (j = 0; j < i; j++)
				brelse (sbi->group_desc[j]);

			err = -ENOMEM;
			hdd_msg(sb, KERN_ERR,__func__,"Unable to read group_desc");
			goto free_gdt_bh;
		}
	}

	if (init_sb_info(sbi) < 0) {		/* 初始化各字段 */
		err = -ENOMEM;
		hdd_msg(sb, KERN_ERR,__func__,"Unable to init per_cpu_count");
		goto free_per_cpu;
	}

	sbi->sb = sb;
	sb->s_fs_info	= sbi;
	sb->s_maxbytes	= hdd_max_size();	/* 最大文件长度 */
	sb->s_flags	= sb->s_flags & ~MS_POSIXACL;/* 取消访问控制 */
	sb->s_op	= &hdd_sops;
	sb->s_export_op = &hdd_export_ops;
	sb->s_xattr	= NULL;

	if (hdd_get_ssd(sbi) < 0){		/* 读取 SSD 信息 */
		hdd_msg(sb, KERN_ERR,__func__,"Unable to get ssd info");
		goto empty_sb_fs;
	}

	root = hdd_iget(sb, HDD_ROOT_INO);	/* 读取根 inode */
	if (IS_ERR(root)) {
		err = PTR_ERR(root);
		hdd_msg(sb, KERN_ERR,__func__,"Unable to read root inode");
		goto release_ssd;
	}

	if (!S_ISDIR(root->i_mode) || !root->i_blocks || !root->i_size) {
		iput(root);
		hdd_msg(sb, KERN_ERR,__func__,"corrupt root inode");
		goto release_ssd;
	}

	sb->s_root = d_alloc_root(root);	/* 分配根目录项 */
	if (!sb->s_root) {
		iput(root);
		err = -ENOMEM;
		hdd_msg(sb, KERN_ERR,__func__,"get root inode failed");
		goto release_ssd;
	}

	/* 检查文件系统 */
	hdd_setup_super(sb, hdd_sb, sb->s_flags & MS_RDONLY);

	return 0;

release_ssd:
	hdd_release_ssd(sbi);

empty_sb_fs:
	sb->s_fs_info = NULL;

free_per_cpu:
	percpu_counter_destroy(&sbi->usr_blocks);
	percpu_counter_destroy(&sbi->free_blks_count);
	percpu_counter_destroy(&sbi->free_inodes_count);
	percpu_counter_destroy(&sbi->total_access);
	percpu_counter_destroy(&sbi->ssd_blks_count);
	for (i = 0; i < FMC_MAX_LEVELS; i++)
		percpu_counter_destroy(&sbi->blks_per_lvl[i]);

release_gdt_bh:
	for (i = 0; i < sbi->gdt_blocks; i++)
		brelse(sbi->group_desc[i]);

free_gdt_bh:
	kfree(sbi->group_desc);

release_hdd_bh:
	brelse(hdd_bh);

free_blks_level:
	kfree(sbi->blks_per_lvl);

free_grplock:
	kfree(sbi->blkgrp_lock);

free_sbi:
	kfree(sbi);
	return err;
}

/* 读取超级块 */
static int hdd_get_sb(struct file_system_type *type, int flags,
	const char *dev_name, void *data, struct vfsmount *mnt)
{
	return get_sb_bdev(type, flags, dev_name, data, hdd_fill_sb, mnt);
}

/* 初始化 inode 私有信息缓存中的一个新元素 */
static void init_once(void *foo)
{
	struct hdd_inode_info *hdi = (struct hdd_inode_info *) foo;

	rwlock_init(&hdi->i_meta_lock);
	mutex_init(&hdi->truncate_mutex);
	
	inode_init_once(&hdi->vfs_inode); /* 初始化 inode 对象 */
}

/* 创建 inode 私有信息缓存 */
static int init_inodecache(void)
{
	hdd_inode_cachep = kmem_cache_create(
		"hdd_inode_cache",
		sizeof(struct hdd_inode_info),
		0, 
		(SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD),
		init_once );

	if (hdd_inode_cachep == NULL)
		return -ENOMEM;

	return 0;	

}

/* 销毁 inode 私有信息缓存 */
static void destroy_inodecache(void)
{
	kmem_cache_destroy(hdd_inode_cachep);
}

/* fmc_hdd 文件系统结构 */
static struct file_system_type hdd_fs_type = {
	.owner	 = THIS_MODULE,
	.name	 = "fmc_hdd",
	.get_sb	 = hdd_get_sb,
	.kill_sb = kill_block_super,
	.fs_flags= FS_REQUIRES_DEV,
};

/* 安装模块 */
static int __init init_hdd_fs(void)
{
	int err;
	err = init_inodecache();/* 创建 inode 私有信息缓存 */
	if (err)
		goto out1;

	err = register_filesystem(&hdd_fs_type);
	if (err)
		goto out;

	return 0;
out:
	destroy_inodecache();/*销毁 inode 私有信息缓存 */
out1:
	return err;
}

/* 卸载模块 */
static void __exit exit_hdd_fs(void)
{
	unregister_filesystem(&hdd_fs_type);
	destroy_inodecache();/*销毁 inode 私有信息缓存 */
}

module_init(init_hdd_fs)
module_exit(exit_hdd_fs)

MODULE_AUTHOR("Xuesen Liang");
MODULE_DESCRIPTION("Flash, Magnetic and Cloud Filesystem");
MODULE_LICENSE("GPL");