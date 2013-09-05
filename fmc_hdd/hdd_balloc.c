/*
 * fmcfs/fmc_hdd/hdd_balloc.c
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * @ Beijing University of Posts and Telecommunications,
 * @ CPU & SoC Center of Tsinghua University.
 *
 * This program can be redistributed under the terms of the GNU Public License.
 */

#include <linux/sched.h>
#include <linux/buffer_head.h>
#include <linux/capability.h>

#include "hdd.h"

/* ȡ��һ������������ַ */
struct hdd_group_desc * hdd_get_group_desc(struct super_block * sb,
	unsigned int block_group, struct buffer_head ** bh)
{
	unsigned int group_desc;
	unsigned int offset;
	struct hdd_group_desc * desc;
	struct hdd_sb_info *sbi = HDD_SB(sb);

	if (block_group >= sbi->groups_count) { /* �ڿ�������Χ�� */
		hdd_msg (sb, KERN_ERR, "hdd_get_group_desc",
			"block_group >= groups_count");
		return NULL;
	}

	group_desc = block_group >> sbi->desc_per_blk_bits;/* ���ڿ� */
	offset = block_group & (sbi->desc_per_block - 1);/* ������� */
	if (!sbi->group_desc[group_desc]) {/* ȷ�ϳ������������������ */
		hdd_msg (sb, KERN_ERR, "hdd_get_group_desc",
			"Group descriptor not loaded");
		return NULL;
	}

	desc = (struct hdd_group_desc *) sbi->group_desc[group_desc]->b_data;
	if (bh)
		*bh = sbi->group_desc[group_desc];

	return desc + offset; /* ���������ṹ����ʼ��ַ */
}

/* �������п��п������ܿ��п��� */
static void group_adjust_blocks(struct super_block *sb, int group_no,
	struct hdd_group_desc *desc, struct buffer_head *bh, int count)
{
	if (count) {
		struct hdd_sb_info *sbi = HDD_SB(sb);
		unsigned free_blocks;

		down_read(&sbi->sbi_rwsem);

		spin_lock(sb_bgl_lock(sbi, group_no)); /* ������п��� */
		free_blocks = le32_to_cpu(desc->bg_free_blocks_count);
		desc->bg_free_blocks_count = cpu_to_le32(free_blocks + count);
		spin_unlock(sb_bgl_lock(sbi, group_no));

		percpu_counter_add(&sbi->free_blks_count, count);/* �ܿ��п��� */

		sb->s_dirt = sbi->s_dirty = 1;
		mark_buffer_dirty(bh);

		up_read(&sbi->sbi_rwsem);
	}
}

/* �ͷ� block ��ʼ�� count ����, �����������Ϣ */
void hdd_free_blocks (struct inode * inode,
	unsigned long block, unsigned long count)
{
	struct buffer_head * bitmap_bh = NULL;
	struct buffer_head * bh2;
	unsigned long block_group;
	unsigned long bit;
	unsigned long i;
	struct super_block * sb = inode->i_sb;
	struct hdd_sb_info * sbi = HDD_SB(sb);
	struct hdd_group_desc * desc;
	struct hdd_super_block * hs = sbi->hdd_sb;
	unsigned group_freed;
	unsigned long nr_in_grp = (block - le32_to_cpu(hs->s_first_data_block))
		% sbi->blks_per_group;
	
	if (block < sbi->grp_data_offset + le32_to_cpu(hs->s_first_data_block)
	||  block + count < block
	||  block + count > le32_to_cpu(hs->s_blocks_count)
	||  nr_in_grp < sbi->grp_data_offset) {
		hdd_msg (sb, KERN_ERR, "hdd_free_blocks",
			    "Freeing blocks not in datazone - "
			    "block = %lu, count = %lu", block, count);
		goto error_return;
	}

	block_group = (block - le32_to_cpu(hs->s_first_data_block)) / /* ����� */
		      sbi->blks_per_group;
	if (block_group == sbi->groups_count - 1
	&&  nr_in_grp + count > sbi->last_group_blks )  /* ���һ������, �����˿��� */
		goto error_return;		
	
	/* �ڿ�λͼ�ڲ��� bit �� */
	bit = (block - le32_to_cpu(hs->s_first_data_block)) %
		      sbi->blks_per_group - sbi->grp_data_offset; 

	/* �������Խ���� */
	if (bit + count > HDD_BLOCK_SIZE) 
		goto error_return;

	bitmap_bh = read_block_bitmap(sb, block_group); /* ����λͼ */
	if (!bitmap_bh)
		goto error_return;

	desc = hdd_get_group_desc (sb, block_group, &bh2);/* ���������� */
	if (!desc)
		goto error_return;

	for (i = 0, group_freed = 0; i < count; i++) { /* ��� bit */
		if (!ext2_clear_bit_atomic(sb_bgl_lock(sbi, block_group),
			bit + i, bitmap_bh->b_data)) {
			hdd_msg(sb, KERN_ERR, __func__,
				"bit already cleared for block %lu", block + i);
		} else {
			group_freed++;
		}
	}

	mark_buffer_dirty(bitmap_bh);	/* ���λͼ�޸� */
	if (sb->s_flags & MS_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);

	/* ���ӿ��п��� */
	group_adjust_blocks(sb, block_group, desc, bh2, group_freed);

error_return:
	brelse(bitmap_bh);
	if (0 != group_freed) {
		spin_lock(&inode->i_lock);
		inode->i_blocks -= group_freed;	/* ���� inode ���� */
		spin_unlock(&inode->i_lock);
	}
}

/* ����ļ�ϵͳ�Ƿ��п��п� */
static int hdd_has_free_blocks(struct hdd_sb_info *sbi)
{
	unsigned int free_blocks = 
		percpu_counter_read_positive(&sbi->free_blks_count);

	if (free_blocks < 1 ) 
			return 0;
	return 1;
}

/* ��ȡ����Ŀ�λͼ */
static struct buffer_head *
read_block_bitmap(struct super_block *sb, unsigned int block_group)
{
	struct hdd_group_desc * desc = NULL;
	struct buffer_head * bh = NULL;
	unsigned int bitmap_blk;

	desc = hdd_get_group_desc(sb, block_group, NULL);/* ��ȡ�������� */
	if (!desc)
		return NULL;

	bitmap_blk = le32_to_cpu(desc->bg_block_bitmap);
	bh = sb_getblk(sb, bitmap_blk);	/* ��ȡ��λͼ�� */
	if (unlikely(!bh)) {
		hdd_msg(sb, KERN_ERR, __func__,
			    "Cannot read block bitmap - "
			    "block_group = %d, block_bitmap = %u",
			    block_group, le32_to_cpu(desc->bg_block_bitmap));
		return NULL;
	}
	if (likely(bh_uptodate_or_lock(bh)))/* ������ */
		return bh;

	if (bh_submit_read(bh) < 0) { /* �����ȡ�� */
		brelse(bh);
		hdd_msg(sb, KERN_ERR, __func__,
			    "Cannot read block bitmap - "
			    "block_group = %d, block_bitmap = %u",
			    block_group, le32_to_cpu(desc->bg_block_bitmap));
		return NULL;
	}

	return bh;
}

/* ���ҵ�һ������ bit */
static int bitmap_search_next_usable_block(int start, 
	struct buffer_head *bh,	int maxblocks)
{
	int next;

	/* ȡ��[data+start(b) ~ data+max(bit)) �е��׸� 0 bit */
	next = find_next_zero_bit(bh->b_data, maxblocks, start);

	if (next >= maxblocks)
		return -1; /* ��Χ��δ�ҵ����� bit */

	return next;
}

/* ��λͼ�в���һ�����ÿ� */
static int find_next_usable_block(int start, 
	struct buffer_head *bh, int maxblocks/*4096*/)
{
	int here = 0, next = 0;
	char *p = NULL, *r = NULL;

	/* ����ͼ��Ŀ��֮���Լ 64 �������� */
	if (start > 0) { 
		int end_goal = (start + 63) & ~63;
		if (end_goal > maxblocks)
			end_goal = maxblocks;

		/* ���ҵ�һ�� '0' bit(���ֽ�, ÿ���ֽڴӵ͵���) */
		here = find_next_zero_bit(bh->b_data, end_goal, start);
		if (here < end_goal)
			return here;
	}

	/* Ȼ����ͼ����һ�������� */
	here = start;
	if (here < 0)
		here = 0;
	p = ((char *)bh->b_data) + (here >> 3);
	r = memscan(p, 0, ((maxblocks + 7) >> 3) - (here >> 3));
	next = (r - ((char *)bh->b_data)) << 3;
	if (next < maxblocks && next >= here)
		return next;

	/* ������������б��� */
	here = bitmap_search_next_usable_block(here, bh, maxblocks);
	return here;
}

/* ���Է��� count ���� */
static int hdd_try_to_alloc(struct super_block *sb, unsigned int group,
	struct buffer_head *bitmap_bh, int grp_goal, unsigned long *count)
{
//@grp_goal: �ڿ����е���Կ��

	unsigned int group_first_block = 0;
       	int start = 0, end = 0;
	unsigned long num = 0;
	int i = 0;
	int offset = HDD_SB(sb)->grp_data_offset;

	if (grp_goal < offset)
		start = offset;
	else
		start = grp_goal;
	start -= offset;

	end = HDD_BLOCKS_PER_GROUP(sb) - offset;

	BUG_ON(start > HDD_BLOCKS_PER_GROUP(sb));

repeat:
	if (grp_goal < 0) { /* δָ��Ŀ��ķ�ʽ���ҿ�λ */
		grp_goal = find_next_usable_block(start, bitmap_bh, end);
		if (grp_goal < 0)
			goto fail_access;/* ʧ���� */

		/* �ҵ���λ����ǰ����, ������û�п�λ */
		i = 0; 
		while (i < 7 && grp_goal > start
		&& !test_bit(grp_goal - 1, bitmap_bh->b_data)){
			i++;
			grp_goal--;
		}
	}

	start = grp_goal;
	/* ���� bit, ������ԭֵ */
	if (ext2_set_bit_atomic(sb_bgl_lock(HDD_SB(sb),	group),
		grp_goal, bitmap_bh->b_data)) {
		start++;
		grp_goal++;
		if (start >= end)
			goto fail_access;/* ʧ���� */
		goto repeat; /* ԭ���ѱ���1, ������, ֱ���ҵ���λ */
	}

	/* �ҵ��� 1 ����λ */
	num++;
	grp_goal++;
	while (num < *count && grp_goal < end
	/* ��������ȥ */
	&& !ext2_set_bit_atomic(sb_bgl_lock(EXT2_SB(sb), group),
		grp_goal, bitmap_bh->b_data)) {
		num++;
		grp_goal++;
	}

	/* count �м�¼�ҵ�����������λ�� */
	*count = num;
	return grp_goal - num;/* �����׸���λ */

fail_access:
	*count = num;
	return -1;
}

/* �����ĺ��ĺ��� - �ӽ���Ŀ�괦���� count ���� */
unsigned int hdd_new_blocks(struct inode *inode,
	unsigned int goal, unsigned long *count, int *errp)
{
/* @goal: �����Ŀ����
 * @count: ��Ҫ����Ŀ���
 * ���ط���Ŀ������е�һ����Ŀ��
 *
 * ���Ŀ������, ����Ŀ����� 32 ��������һ���п�, ������Ǹ���;
 * ����, �����������п�: �ڿ�����,	> ������λͼ��һ�������ֽ�, 
 *				> ��ʧ��, ����������� bit.
 * This function also updates quota and i_blocks field.
 */
	struct buffer_head *bitmap_bh = NULL;
	struct buffer_head *gdp_bh;
	int group_no;
	int goal_group;
	int grp_target_blk;		/* blockgroup relative goal block */
	int grp_alloc_blk;		/* blockgroup-relative allocated block*/
	unsigned int ret_block;		/* filesyetem-wide allocated block */
	int bgi;			/* blockgroup iteration index */
	int performed_allocation = 0;
	int free_blocks;	/* number of free blocks in a group */
	struct super_block *sb;
	struct hdd_group_desc *gdp;
	struct hdd_super_block *hs;
	struct hdd_sb_info *sbi;
	unsigned long ngroups;
	unsigned long num = *count;
	unsigned long remain = 0;

	*errp = -ENOSPC;
	sb = inode->i_sb;
	if (!sb) {
		printk("hdd_new_blocks: nonexistent device");
		return 0;
	}

	sbi = HDD_SB(sb);
	hs = HDD_SB(sb)->hdd_sb;
	fmc_debug("goal = %u.\n", goal);

	if (!hdd_has_free_blocks(sbi)) {	/* ȷ���Ƿ��п��п� */
		*errp = -ENOSPC;
		goto out;
	}

	/* ���ȼ�齨��Ŀ����Ƿ�Ϸ� */
	if (goal < le32_to_cpu(hs->s_first_data_block) ||
	    goal >= le32_to_cpu(hs->s_blocks_count))
		goal = sbi->grp_data_offset
		 + le32_to_cpu(hs->s_first_data_block);

	/* ����Ŀ��������Ŀ��� */
	group_no = (goal - le32_to_cpu(hs->s_first_data_block)) /
		sbi->blks_per_group;
	goal_group = group_no;/* Ŀ����� */

	gdp = hdd_get_group_desc(sb, group_no, &gdp_bh); /* ȡ���������� */
	if (!gdp)
		goto io_error;

	free_blocks = le32_to_cpu(gdp->bg_free_blocks_count);/* ���п��� */

	/* �����п��п� */
	if (free_blocks > 0) {
		/* ����������Ե�Ŀ���� */
		grp_target_blk = ((goal - le32_to_cpu(hs->s_first_data_block))
			% sbi->blks_per_group);

		bitmap_bh = read_block_bitmap(sb, group_no);/* ȡ�ÿ�λͼ */
		if (!bitmap_bh)
			goto io_error;

		/* ���Խ��з�������Ŀ��� */
		grp_alloc_blk = hdd_try_to_alloc(sb, group_no,
				bitmap_bh, grp_target_blk, &num);
		if (grp_alloc_blk >= 0)
			goto allocated;/* ������ */
	}

	/* ������û�п��п�, ��û�з���ɹ� */
	ngroups = sbi->groups_count;
	smp_rmb();

	/* �������������п��� */
	for (bgi = 0; bgi < ngroups; bgi++) {
		group_no++;
		if (group_no >= ngroups)
			group_no = 0;
		gdp = hdd_get_group_desc(sb, group_no, &gdp_bh); /* ȡ���������� */
		if (!gdp)
			goto io_error;

		free_blocks = le32_to_cpu(gdp->bg_free_blocks_count);

		brelse(bitmap_bh);
		bitmap_bh = read_block_bitmap(sb, group_no);/* ȡ�ÿ�λͼ */
		if (!bitmap_bh)
			goto io_error;

		/* �Բ�ָ���ڴ�Ŀ��ķ�ʽ���Է��� */
		grp_alloc_blk = hdd_try_to_alloc(sb, group_no,
					bitmap_bh, -1, &num);
		if (grp_alloc_blk >= 0)
			goto allocated;
	}

	/* �豸��û�п��п� */
	*errp = -ENOSPC;
	goto out;

allocated: /* ����ɹ�������ͳ����Ϣ��Ȼ�󷵻� */

	fmc_debug("using block group %d(%d)\n",
			group_no, gdp->bg_free_blocks_count);

	ret_block = grp_alloc_blk + hdd_group_first_block_no(sb, group_no)
		+ sbi->grp_data_offset;

	performed_allocation = 1;

	if (ret_block + num - 1 >= le32_to_cpu(hs->s_blocks_count)) {
		hdd_msg(sb, KERN_ERR, "hdd_new_blocks",
			    "block(%u) >= blocks count(%d) - "
			    "block_group = %d, hs == %p ", ret_block,
			le32_to_cpu(hs->s_blocks_count), group_no, hs);
		goto out;
	}

	group_adjust_blocks(sb, group_no, gdp, gdp_bh, -num); /* ��������п��� */

	spin_lock(&inode->i_lock);
	inode->i_blocks += num;		/* ���� inode ���� */
	spin_unlock(&inode->i_lock);

	mark_buffer_dirty(bitmap_bh);	/* �������������Ϊ�� */
	if (sb->s_flags & MS_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);

	*errp = 0;
	brelse(bitmap_bh); /* �ͷ� buffer */

	*count = num; /* ��¼�ѷ������ */

	return ret_block; /* ������ʼ��� */

io_error:
	*errp = -EIO;
out:
	brelse(bitmap_bh);
	return 0;
}