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

/* 取得一个组描述符地址 */
struct hdd_group_desc * hdd_get_group_desc(struct super_block * sb,
	unsigned int block_group, struct buffer_head ** bh)
{
	unsigned int group_desc;
	unsigned int offset;
	struct hdd_group_desc * desc;
	struct hdd_sb_info *sbi = HDD_SB(sb);

	if (block_group >= sbi->groups_count) { /* 在块组数范围内 */
		hdd_msg (sb, KERN_ERR, "hdd_get_group_desc",
			"block_group >= groups_count");
		return NULL;
	}

	group_desc = block_group >> sbi->desc_per_blk_bits;/* 所在块 */
	offset = block_group & (sbi->desc_per_block - 1);/* 块中序号 */
	if (!sbi->group_desc[group_desc]) {/* 确认超级块加载了组描述符 */
		hdd_msg (sb, KERN_ERR, "hdd_get_group_desc",
			"Group descriptor not loaded");
		return NULL;
	}

	desc = (struct hdd_group_desc *) sbi->group_desc[group_desc]->b_data;
	if (bh)
		*bh = sbi->group_desc[group_desc];

	return desc + offset; /* 组描述符结构的起始地址 */
}

/* 调整组中空闲块数和总空闲块数 */
static void group_adjust_blocks(struct super_block *sb, int group_no,
	struct hdd_group_desc *desc, struct buffer_head *bh, int count)
{
	if (count) {
		struct hdd_sb_info *sbi = HDD_SB(sb);
		unsigned free_blocks;

		down_read(&sbi->sbi_rwsem);

		spin_lock(sb_bgl_lock(sbi, group_no)); /* 块组空闲块数 */
		free_blocks = le32_to_cpu(desc->bg_free_blocks_count);
		desc->bg_free_blocks_count = cpu_to_le32(free_blocks + count);
		spin_unlock(sb_bgl_lock(sbi, group_no));

		percpu_counter_add(&sbi->free_blks_count, count);/* 总空闲块数 */

		sb->s_dirt = sbi->s_dirty = 1;
		mark_buffer_dirty(bh);

		up_read(&sbi->sbi_rwsem);
	}
}

/* 释放 block 开始的 count 个块, 并更新相关信息 */
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

	block_group = (block - le32_to_cpu(hs->s_first_data_block)) / /* 块组号 */
		      sbi->blks_per_group;
	if (block_group == sbi->groups_count - 1
	&&  nr_in_grp + count > sbi->last_group_blks )  /* 最后一个块组, 超过了块数 */
		goto error_return;		
	
	/* 在块位图内部的 bit 号 */
	bit = (block - le32_to_cpu(hs->s_first_data_block)) %
		      sbi->blks_per_group - sbi->grp_data_offset; 

	/* 不允许跨越块组 */
	if (bit + count > HDD_BLOCK_SIZE) 
		goto error_return;

	bitmap_bh = read_block_bitmap(sb, block_group); /* 读块位图 */
	if (!bitmap_bh)
		goto error_return;

	desc = hdd_get_group_desc (sb, block_group, &bh2);/* 读组描述符 */
	if (!desc)
		goto error_return;

	for (i = 0, group_freed = 0; i < count; i++) { /* 清除 bit */
		if (!ext2_clear_bit_atomic(sb_bgl_lock(sbi, block_group),
			bit + i, bitmap_bh->b_data)) {
			hdd_msg(sb, KERN_ERR, __func__,
				"bit already cleared for block %lu", block + i);
		} else {
			group_freed++;
		}
	}

	mark_buffer_dirty(bitmap_bh);	/* 标记位图修改 */
	if (sb->s_flags & MS_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);

	/* 增加空闲块数 */
	group_adjust_blocks(sb, block_group, desc, bh2, group_freed);

error_return:
	brelse(bitmap_bh);
	if (0 != group_freed) {
		spin_lock(&inode->i_lock);
		inode->i_blocks -= group_freed;	/* 减少 inode 块数 */
		spin_unlock(&inode->i_lock);
	}
}

/* 检查文件系统是否有空闲块 */
static int hdd_has_free_blocks(struct hdd_sb_info *sbi)
{
	unsigned int free_blocks = 
		percpu_counter_read_positive(&sbi->free_blks_count);

	if (free_blocks < 1 ) 
			return 0;
	return 1;
}

/* 读取块组的块位图 */
static struct buffer_head *
read_block_bitmap(struct super_block *sb, unsigned int block_group)
{
	struct hdd_group_desc * desc = NULL;
	struct buffer_head * bh = NULL;
	unsigned int bitmap_blk;

	desc = hdd_get_group_desc(sb, block_group, NULL);/* 读取组描述符 */
	if (!desc)
		return NULL;

	bitmap_blk = le32_to_cpu(desc->bg_block_bitmap);
	bh = sb_getblk(sb, bitmap_blk);	/* 读取块位图块 */
	if (unlikely(!bh)) {
		hdd_msg(sb, KERN_ERR, __func__,
			    "Cannot read block bitmap - "
			    "block_group = %d, block_bitmap = %u",
			    block_group, le32_to_cpu(desc->bg_block_bitmap));
		return NULL;
	}
	if (likely(bh_uptodate_or_lock(bh)))/* 块最新 */
		return bh;

	if (bh_submit_read(bh) < 0) { /* 否则读取块 */
		brelse(bh);
		hdd_msg(sb, KERN_ERR, __func__,
			    "Cannot read block bitmap - "
			    "block_group = %d, block_bitmap = %u",
			    block_group, le32_to_cpu(desc->bg_block_bitmap));
		return NULL;
	}

	return bh;
}

/* 查找第一个空闲 bit */
static int bitmap_search_next_usable_block(int start, 
	struct buffer_head *bh,	int maxblocks)
{
	int next;

	/* 取得[data+start(b) ~ data+max(bit)) 中的首个 0 bit */
	next = find_next_zero_bit(bh->b_data, maxblocks, start);

	if (next >= maxblocks)
		return -1; /* 范围内未找到空闲 bit */

	return next;
}

/* 从位图中查找一个可用块 */
static int find_next_usable_block(int start, 
	struct buffer_head *bh, int maxblocks/*4096*/)
{
	int here = 0, next = 0;
	char *p = NULL, *r = NULL;

	/* 先试图在目标之后的约 64 个块内找 */
	if (start > 0) { 
		int end_goal = (start + 63) & ~63;
		if (end_goal > maxblocks)
			end_goal = maxblocks;

		/* 查找第一个 '0' bit(按字节, 每个字节从低到高) */
		here = find_next_zero_bit(bh->b_data, end_goal, start);
		if (here < end_goal)
			return here;
	}

	/* 然后试图查找一个空闲字 */
	here = start;
	if (here < 0)
		here = 0;
	p = ((char *)bh->b_data) + (here >> 3);
	r = memscan(p, 0, ((maxblocks + 7) >> 3) - (here >> 3));
	next = (r - ((char *)bh->b_data)) << 3;
	if (next < maxblocks && next >= here)
		return next;

	/* 最后查找任意空闲比特 */
	here = bitmap_search_next_usable_block(here, bh, maxblocks);
	return here;
}

/* 尝试分配 count 个块 */
static int hdd_try_to_alloc(struct super_block *sb, unsigned int group,
	struct buffer_head *bitmap_bh, int grp_goal, unsigned long *count)
{
//@grp_goal: 在块组中的相对块号

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
	if (grp_goal < 0) { /* 未指定目标的方式查找空位 */
		grp_goal = find_next_usable_block(start, bitmap_bh, end);
		if (grp_goal < 0)
			goto fail_access;/* 失败了 */

		/* 找到空位后向前找找, 看还有没有空位 */
		i = 0; 
		while (i < 7 && grp_goal > start
		&& !test_bit(grp_goal - 1, bitmap_bh->b_data)){
			i++;
			grp_goal--;
		}
	}

	start = grp_goal;
	/* 设置 bit, 并返回原值 */
	if (ext2_set_bit_atomic(sb_bgl_lock(HDD_SB(sb),	group),
		grp_goal, bitmap_bh->b_data)) {
		start++;
		grp_goal++;
		if (start >= end)
			goto fail_access;/* 失败了 */
		goto repeat; /* 原来已被置1, 则重来, 直到找到空位 */
	}

	/* 找到了 1 个空位 */
	num++;
	grp_goal++;
	while (num < *count && grp_goal < end
	/* 连续找下去 */
	&& !ext2_set_bit_atomic(sb_bgl_lock(EXT2_SB(sb), group),
		grp_goal, bitmap_bh->b_data)) {
		num++;
		grp_goal++;
	}

	/* count 中记录找到的连续空闲位数 */
	*count = num;
	return grp_goal - num;/* 返回首个空位 */

fail_access:
	*count = num;
	return -1;
}

/* 块分配的核心函数 - 从建议目标处分配 count 个块 */
unsigned int hdd_new_blocks(struct inode *inode,
	unsigned int goal, unsigned long *count, int *errp)
{
/* @goal: 建议的目标块号
 * @count: 需要分配的块数
 * 返回分配的块序列中第一个块的块号
 *
 * 如果目标块空闲, 或者目标块后的 32 个块内有一空闲块, 则分配那个块;
 * 否则, 依次搜索空闲块: 在块组中,	> 首先找位图的一个空闲字节, 
 *				> 若失败, 则找任意空闲 bit.
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

	if (!hdd_has_free_blocks(sbi)) {	/* 确认是否还有空闲块 */
		*errp = -ENOSPC;
		goto out;
	}

	/* 首先检查建议目标块是否合法 */
	if (goal < le32_to_cpu(hs->s_first_data_block) ||
	    goal >= le32_to_cpu(hs->s_blocks_count))
		goal = sbi->grp_data_offset
		 + le32_to_cpu(hs->s_first_data_block);

	/* 计算目标块所属的块组 */
	group_no = (goal - le32_to_cpu(hs->s_first_data_block)) /
		sbi->blks_per_group;
	goal_group = group_no;/* 目标块组 */

	gdp = hdd_get_group_desc(sb, group_no, &gdp_bh); /* 取得组描述符 */
	if (!gdp)
		goto io_error;

	free_blocks = le32_to_cpu(gdp->bg_free_blocks_count);/* 空闲块数 */

	/* 组中有空闲块 */
	if (free_blocks > 0) {
		/* 计算组中相对的目标块号 */
		grp_target_blk = ((goal - le32_to_cpu(hs->s_first_data_block))
			% sbi->blks_per_group);

		bitmap_bh = read_block_bitmap(sb, group_no);/* 取得块位图 */
		if (!bitmap_bh)
			goto io_error;

		/* 尝试进行分配所需的块数 */
		grp_alloc_blk = hdd_try_to_alloc(sb, group_no,
				bitmap_bh, grp_target_blk, &num);
		if (grp_alloc_blk >= 0)
			goto allocated;/* 分配了 */
	}

	/* 块组中没有空闲块, 或没有分配成功 */
	ngroups = sbi->groups_count;
	smp_rmb();

	/* 尝试其他的所有块组 */
	for (bgi = 0; bgi < ngroups; bgi++) {
		group_no++;
		if (group_no >= ngroups)
			group_no = 0;
		gdp = hdd_get_group_desc(sb, group_no, &gdp_bh); /* 取得组描述符 */
		if (!gdp)
			goto io_error;

		free_blocks = le32_to_cpu(gdp->bg_free_blocks_count);

		brelse(bitmap_bh);
		bitmap_bh = read_block_bitmap(sb, group_no);/* 取得块位图 */
		if (!bitmap_bh)
			goto io_error;

		/* 以不指定期待目标的方式尝试分配 */
		grp_alloc_blk = hdd_try_to_alloc(sb, group_no,
					bitmap_bh, -1, &num);
		if (grp_alloc_blk >= 0)
			goto allocated;
	}

	/* 设备中没有空闲块 */
	*errp = -ENOSPC;
	goto out;

allocated: /* 分配成功，更新统计信息，然后返回 */

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

	group_adjust_blocks(sb, group_no, gdp, gdp_bh, -num); /* 减少组空闲块数 */

	spin_lock(&inode->i_lock);
	inode->i_blocks += num;		/* 增加 inode 块数 */
	spin_unlock(&inode->i_lock);

	mark_buffer_dirty(bitmap_bh);	/* 标记组描述符块为脏 */
	if (sb->s_flags & MS_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);

	*errp = 0;
	brelse(bitmap_bh); /* 释放 buffer */

	*count = num; /* 记录已分配块数 */

	return ret_block; /* 返回起始块号 */

io_error:
	*errp = -EIO;
out:
	brelse(bitmap_bh);
	return 0;
}