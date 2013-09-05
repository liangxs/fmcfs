/*
 * linux/fs/fmc/hdd_dir.c
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * Beijing University of Posts and Telecommunications,
 * and CPU Center @ Tsinghua University.
 *
 * from linux/fs/ext2/ialloc.c
 *
 * This program can be redistributed under the terms of the GNU Public License.
 */

#include <linux/quotaops.h>
#include <linux/sched.h>
#include <linux/backing-dev.h>
#include <linux/buffer_head.h>
#include <linux/random.h>

#include "fmc_fs.h"
#include "hdd.h"

/* ȷ��Ŀ¼�ļ� inode ���ڵĿ��� */
static int find_group_orlov(struct super_block *sb, struct inode *parent)
{
	int parent_group = HDD_I(parent)->i_block_group;
	struct hdd_sb_info *sbi = HDD_SB(sb);
	struct hdd_super_block *hs = sbi->hdd_sb;
	int ngroups = sbi->groups_count;
	int inodes_per_group = sbi->inodes_per_group;
	struct hdd_group_desc *desc;
	int freei;
	int avefreei;
	int free_blocks;
	int avefreeb;
	int blocks_per_dir;
	//int ndirs;
	int max_debt, max_dirs, min_blocks, min_inodes;
	int group = -1, i;

	freei = percpu_counter_read_positive(&sbi->free_inodes_count);
	avefreei = freei / ngroups;
	free_blocks = percpu_counter_read_positive(&sbi->free_blks_count);
	avefreeb = free_blocks / ngroups;

	if ((parent == sb->s_root->d_inode)	/* ��Ŀ¼Ϊ��Ŀ¼ */
	||  (HDD_I(parent)->i_flags & HDD_TOPDIR_FL)) {
		struct hdd_group_desc *best_desc = NULL;
		int best_ndir = inodes_per_group;
		int best_group = -1;

		get_random_bytes(&group, sizeof(group)); /* ����������� */
		parent_group = (unsigned)group % ngroups;

		for (i = 0; i < ngroups; i++) {
			group = (parent_group + i) % ngroups;
			desc = hdd_get_group_desc (sb, group, NULL);
			if (!desc || !desc->bg_free_inodes_count)
				continue;
			if (le32_to_cpu(desc->bg_used_dirs_count) >= best_ndir)
				continue;
			if (le32_to_cpu(desc->bg_free_inodes_count) < avefreei)
				continue;
			if (le32_to_cpu(desc->bg_free_blocks_count) < avefreeb)
				continue;
			best_group = group;
			best_ndir = le32_to_cpu(desc->bg_used_dirs_count);
			best_desc = desc;
		}

		if (best_group >= 0) {
			desc = best_desc;
			group = best_group;
			goto found; /* �ҵ� */
		}
		goto fallback;
	}

	/* ��Ŀ¼֮���Ŀ¼ */
	min_inodes = avefreei - inodes_per_group / 4;
	min_blocks = avefreeb - sbi->blks_per_group / 4;

	for (i = 0; i < ngroups; i++) {
		group = (parent_group + i) % ngroups;
		desc = hdd_get_group_desc (sb, group, NULL);
		if (!desc || !desc->bg_free_inodes_count)
			continue;
		if (le32_to_cpu(desc->bg_used_dirs_count) >= max_dirs)
			continue;
		if (le32_to_cpu(desc->bg_free_inodes_count) < min_inodes)
			continue;
		if (le32_to_cpu(desc->bg_free_blocks_count) < min_blocks)
			continue;
		goto found;
	}

fallback:
	for (i = 0; i < ngroups; i++) {
		group = (parent_group + i) % ngroups;
		desc = hdd_get_group_desc (sb, group, NULL);
		if (!desc || !desc->bg_free_inodes_count)
			continue;
		if (le32_to_cpu(desc->bg_free_inodes_count) >= avefreei)
			goto found;
	}

	if (avefreei) {
		avefreei = 0;
		goto fallback;
	}

	return -1;

found:
	return group;
}

/* ȷ����Ŀ¼�ļ� inode ���ڵĿ��� */
static int find_group_other(struct super_block *sb, struct inode *parent)
{
	int parent_group = HDD_I(parent)->i_block_group; /* �� inode �Ŀ��� */
	int ngroups = HDD_SB(sb)->groups_count; /* ������� */
	struct hdd_group_desc *desc;
	int group, i;

	/* ���ȳ��Է��ڸ�Ŀ¼�Ŀ����� */
	group = parent_group;
	desc = hdd_get_group_desc (sb, group, NULL);
	if (desc && le32_to_cpu(desc->bg_free_inodes_count)
		 && le32_to_cpu(desc->bg_free_blocks_count))
		goto found;

	/* û�ҵ�, ���Բ�ͬ����. ��ϣ��ͬĿ¼�µ� inode ��ͬһ�������� */
	group = (group + parent->i_ino) % ngroups; /* ���ݸ� inode ȷ��������ʼ�� */
	for (i = 1; i < ngroups; i <<= 1) { /* ���ö���ɢ�н���ǰ������ */
		group += i;
		if (group >= ngroups)
			group -= ngroups;
		desc = hdd_get_group_desc (sb, group, NULL);
		if (desc && le32_to_cpu(desc->bg_free_inodes_count)
			 && le32_to_cpu(desc->bg_free_blocks_count))
			goto found;
	}

	/* ��ʧ��, ��������Բ��� */
	group = parent_group;
	for (i = 0; i < ngroups; i++) {
		if (++group >= ngroups)
			group = 0;
		desc = hdd_get_group_desc (sb, group, NULL);
		if (desc && le32_to_cpu(desc->bg_free_inodes_count))
			goto found;
	}

	return -1;

found:
	return group;
}

/* ��ȡ�����е� imap */
static struct buffer_head *
read_inode_bitmap(struct super_block * sb, unsigned long block_group)
{
	struct hdd_group_desc *desc;
	struct buffer_head *bh = NULL;

	desc = hdd_get_group_desc(sb, block_group, NULL);
	if (!desc)
		goto error_out;

	bh = sb_bread(sb, le32_to_cpu(desc->bg_inode_bitmap));
	if (!bh)
		hdd_msg(sb, KERN_ERR, "read_inode_bitmap",
			    "Cannot read inode bitmap - "
			    "block_group = %lu, inode_bitmap = %u",
			    block_group, le32_to_cpu(desc->bg_inode_bitmap));
error_out:
	return bh;
}

/* ���豸����ʱ, Ԥ�� inode �ṹ */
static void hdd_preread_inode(struct inode *inode)
{
	unsigned long block_group;
	unsigned long offset;
	unsigned long block;
	struct hdd_group_desc * gdp;
	struct backing_dev_info *bdi;

	/* �ж��Ƿ������豸æ */
	bdi = inode->i_mapping->backing_dev_info;
	if (bdi_read_congested(bdi))
		return;
	if (bdi_write_congested(bdi))
		return;

	/* ��ȡ���ڿ��������� */
	block_group = (inode->i_ino) / HDD_INODES_PER_GROUP(inode->i_sb);
	gdp = hdd_get_group_desc(inode->i_sb, block_group, NULL);
	if (gdp == NULL)
		return;

	/* inode �ڱ��е�ƫ���� */
	offset = ((inode->i_ino) % HDD_INODES_PER_GROUP(inode->i_sb))
		* HDD_INODE_SIZE;

	block = le32_to_cpu(gdp->bg_inode_table) /* inode ���ڿ�� */
		+ (offset >> HDD_BLOCK_LOG_SIZE);

	sb_breadahead(inode->i_sb, block); /* Ԥ�� */
}

/*�ڴ����Ϸ���һ���µ� inode, ���� inode ����ĵ�ַ�� NULL */
struct inode *hdd_new_inode(struct inode *dir, int mode)
{
/* dir: ��Ŀ¼�ļ� inode */

	struct super_block *sb;
	struct buffer_head *bitmap_bh = NULL;
	struct buffer_head *bh2;
	int group, i;
	ino_t ino = 0;
	struct inode * inode;
	struct hdd_group_desc *gdp;
	struct hdd_super_block *hs;
	struct hdd_inode_info *hi;
	struct hdd_sb_info *sbi;
	int err;

	sb = dir->i_sb;
	inode = new_inode(sb); /* ���ڴ��з��� inode.(sb->s_op->alloc_inode) */
	if (!inode)
		return ERR_PTR(-ENOMEM);

	hi = HDD_I(inode);
	sbi = HDD_SB(sb);
	hs = sbi->hdd_sb;

	if (S_ISDIR(mode))	/* ȷ�� inode Ӧ�ڵĿ��� */
		group = find_group_orlov(sb, dir); /* Ŀ¼ inode, Orlov ���� */
	else 
		group = find_group_other(sb, dir); /* ��Ŀ¼ inode */

	if (group == -1) {
		err = -ENOSPC;
		goto fail;
	}

	/* ��ȡ�����λͼ, �����ҿ���λ */
	for (i = 0; i < sbi->groups_count; i++) {
		gdp = hdd_get_group_desc(sb, group, &bh2);
		brelse(bitmap_bh);
		bitmap_bh = read_inode_bitmap(sb, group);
		if (!bitmap_bh) {
			err = -EIO;
			goto fail;
		}
		ino = 0;

repeat_in_this_group: /* ����λͼ�е��׸�����bit,����λ */
		ino = find_next_zero_bit((unsigned long *)bitmap_bh->b_data,
					      HDD_INODES_PER_GROUP(sb), ino);
		if (ino >= HDD_INODES_PER_GROUP(sb)) {
			/* ��������û�п�λ, ��ֱ������һ���� */
			if (++group == sbi->groups_count)
				group = 0;
			continue;
		}
		if (ext2_set_bit_atomic(sb_bgl_lock(sbi, group),
					ino, bitmap_bh->b_data)) {
			/* inode ���������� */
			if (++ino >= HDD_INODES_PER_GROUP(sb)) {
				if (++group == sbi->groups_count)
					group = 0;
				continue;
			}
			/* try to find free inode in the same group */
			goto repeat_in_this_group;
		}
		goto got;
	}

	err = -ENOSPC;
	goto fail;
got:
	/* ���λͼ����Ϊ�� */
	mark_buffer_dirty(bitmap_bh);
	if (sb->s_flags & MS_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);/* д�� imap */
	brelse(bitmap_bh);

	/* ��� ino �Ƿ�Ϸ� */
	ino += group * HDD_INODES_PER_GROUP(sb);
	if (ino < HDD_FIRST_INO || ino >= le32_to_cpu(hs->s_inodes_count)) {
		hdd_msg (sb, KERN_ERR, "hdd_new_inode",
			    "reserved inode or inode > inodes count - "
			    "block_group = %d,inode=%lu", group,
			    (unsigned long) ino);
		err = -EIO;
		goto fail;
	}

	down_read(&sbi->sbi_rwsem);

	percpu_counter_add(&sbi->free_inodes_count, -1);/* �����ܿ��� inode ���� */

	spin_lock(sb_bgl_lock(sbi, group));
	le32_add_cpu(&gdp->bg_free_inodes_count, -1);	/* ��������� inode ���� */
	if (S_ISDIR(mode)) {
		le32_add_cpu(&gdp->bg_used_dirs_count, 1);/* ������Ŀ¼���� */
	}
	spin_unlock(sb_bgl_lock(sbi, group));

	sb->s_dirt = 1; /* ���ó��������Ϊ�� */

	mark_buffer_dirty(bh2); /* ������Ӧ�����Ϊ�� */

	up_read(&sbi->sbi_rwsem);


	/* ��ʼ�� inode */
	inode->i_uid = current_fsuid();
	if (test_opt (sb, GRPID))
		inode->i_gid = dir->i_gid;
	else if (dir->i_mode & S_ISGID) {
		inode->i_gid = dir->i_gid;
		if (S_ISDIR(mode))
			mode |= S_ISGID;
	} else
		inode->i_gid = current_fsgid();
	inode->i_mode = mode;

	inode->i_ino = ino; /* ��� */
	inode->i_blocks = 0;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME_SEC;

	memset(hi->i_data, 0, sizeof(hi->i_data));
	hi->i_state = HDD_STATE_NEW;
	hi->i_flags =
		hdd_mask_flags(mode, HDD_I(dir)->i_flags & HDD_FL_INHERITED);
	hi->i_dtime = 0;
	hi->i_block_group = group;
	hi->i_dir_start_lookup = 0;

	hi->i_access_count = 0;		/* ���ʼ��� */
	hi->i_ssd_blocks = 0;		/* �� SSD �еĿ��� */
	hi->i_direct_bits = 0;		/* ǰ12�����λ�ñ�־ */
	memset(hi->i_direct_blks, 0, HDD_NDIR_BLOCKS); /* ǰ12����ķ��ʼ��� */

	hdd_set_inode_flags(inode);
	inode->i_generation = 0;

	if (insert_inode_locked(inode) < 0) { /* �� inode ���� inode_hashtable */
		err = -EINVAL;
		goto fail_drop;
	}

	mark_inode_dirty(inode); /* �� inode �ŵ���������� inode ������ */
	fmc_debug("allocating inode %lu\n", inode->i_ino);

	/* �Ӵ��̶�ȡ������ inode �Ŀ鵽ҳ�������� */
	hdd_preread_inode(inode);
	return inode;

fail_drop:
	inode->i_flags |= S_NOQUOTA;
	inode->i_nlink = 0;
	unlock_new_inode(inode);
	iput(inode);
	return ERR_PTR(err);

fail:
	make_bad_inode(inode);
	iput(inode);
	return ERR_PTR(err);
}

/* �ӿ������ͷ� inode */
static void hdd_release_inode(struct super_block *sb, int group, int dir)
{
	struct hdd_group_desc * desc;
	struct buffer_head *bh;

	desc = hdd_get_group_desc(sb, group, &bh);
	if (!desc) {
		hdd_msg(sb, KERN_ERR, "hdd_release_inode",
			"can't get descriptor for group %d", group);
		return;
	}

	down_read(&HDD_SB(sb)->sbi_rwsem);

	spin_lock(sb_bgl_lock(HDD_SB(sb), group));
	le32_add_cpu(&desc->bg_free_inodes_count, 1);
	if (dir)
		le32_add_cpu(&desc->bg_used_dirs_count, -1);
	spin_unlock(sb_bgl_lock(HDD_SB(sb), group));

	percpu_counter_inc(&HDD_SB(sb)->free_inodes_count); /* ���� inode �� */
	sb->s_dirt = HDD_SB(sb)->s_dirty = 1;

	up_read(&HDD_SB(sb)->sbi_rwsem);

	mark_buffer_dirty(bh);
}

/* �ͷ� inode */
void hdd_free_inode (struct inode * inode)
{
	struct super_block * sb = inode->i_sb;
	int is_directory;
	unsigned long ino;
	struct buffer_head *bitmap_bh = NULL;
	unsigned long block_group;
	unsigned long bit;
	struct hdd_super_block * hs;

	ino = inode->i_ino;
	fmc_debug ("freeing inode %lu\n", ino);

	hs = HDD_SB(sb)->hdd_sb;
	is_directory = S_ISDIR(inode->i_mode);

	clear_inode (inode);	/* �ͷ��ڴ� inode ���� */

	if (ino < HDD_FIRST_INO || /* inode �Ϸ��� */
	    ino >= le32_to_cpu(hs->s_inodes_count)) {
		hdd_msg(sb, KERN_ERR, "hdd_free_inode",
			    "reserved or nonexistent inode %lu", ino);
		goto error_return;
	}
	
	block_group = ino / HDD_INODES_PER_GROUP;  /* ���� inode ���ڿ��� */
	bit = ino % HDD_INODES_PER_GROUP;

	bitmap_bh = read_inode_bitmap(sb, block_group);  /* ��ȡ inode λͼ */
	if (!bitmap_bh)
		goto error_return;

	if (!ext2_clear_bit_atomic(sb_bgl_lock(HDD_SB(sb), block_group),
				bit, (void *) bitmap_bh->b_data)) /* ��� bit λ */
		hdd_msg(sb, KERN_ERR, "hdd_free_inode",
			      "bit already cleared for inode %lu", ino);
	else
		hdd_release_inode(sb, block_group, is_directory); /* �ͷ� inode */

	mark_buffer_dirty(bitmap_bh); /* ����λͼΪ�� */
	if (sb->s_flags & MS_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh); /* ����ͬ������ */
error_return:
	brelse(bitmap_bh);
}