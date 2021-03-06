/*
 * fmcfs/fmc_hdd/hdd_inode.c
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * @ Beijing University of Posts and Telecommunications,
 * @ CPU & SoC Center of Tsinghua University.
 *
 * This program can be redistributed under the terms of the GNU Public License.
 */

#include <linux/smp_lock.h>
#include <linux/time.h>
#include <linux/highuid.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/fiemap.h>
#include <linux/namei.h>

#include "hdd.h"

typedef struct _inderect {
	__le32	*p;		/* 指向一个块号 */
	__le32	key;		/* 复制了指向的块号值 */
	struct buffer_head *bh; /* 块号对应的块内容 */
} Indirect;

int hdd_sync_inode(struct inode *inode);

void access_info_init(struct inode *inode,struct buffer_head *bh, unsigned int offset);
int access_info_inc(struct inode * inode, Indirect *branch, unsigned int offset);
void access_info_sub(struct inode *inode, __le32 *data, int offset, int count);

/* 从磁盘读取 inode 结构 */
static struct hdd_inode *hdd_get_inode(struct super_block *sb,
	ino_t ino, struct buffer_head **p)
{
	struct buffer_head * bh;
	unsigned int block_group;
	unsigned int block;
	unsigned int offset;
	struct hdd_group_desc * desc;

	*p = NULL;
	if ((ino != HDD_ROOT_INO && ino < HDD_FIRST_INO) /* 确认 ino 合法 */
	||   ino >= HDD_SB(sb)->inodes_count)
		goto Einval;

	block_group = ino / HDD_SB(sb)->inodes_per_group;
	desc = hdd_get_group_desc(sb, block_group, NULL);/* 取组描述符 */
	if (!desc)
		goto Edesc;

	/* inode 结构的偏移量和所在块号 */
	offset = (ino % HDD_SB(sb)->inodes_per_group) * HDD_INODE_SIZE;
	block = le32_to_cpu(desc->bg_inode_table) +
		(offset >> HDD_BLOCK_LOG_SIZE);

	if (!(bh = sb_bread(sb, block)))	/* 读取 inode 所在块 */
		goto Eio;

	*p = bh;
	offset &= (HDD_BLOCK_SIZE - 1);		/* 在块中的起始地址 */
	return (struct hdd_inode *) (bh->b_data + offset);

Einval:
	hdd_msg(sb, KERN_WARNING, "hdd_get_inode", 
		"bad inode number: %lu", (unsigned long) ino);
	return ERR_PTR(-EINVAL);
Eio:
	hdd_msg(sb, KERN_ERR, "hdd_get_inode",
		"unable to read inode - inode=%lu", (unsigned long) ino);
Edesc:
	return ERR_PTR(-EIO);
}

/* 设置 inode 对象的标志 */
void hdd_set_inode_flags(struct inode *inode)
{
	unsigned int flags = HDD_I(inode)->i_flags;

	inode->i_flags &= ~(S_SYNC|S_APPEND|S_IMMUTABLE|S_NOATIME|S_DIRSYNC);

	if (flags & HDD_SYNC_FL)
		inode->i_flags |= S_SYNC;
	if (flags & HDD_APPEND_FL)
		inode->i_flags |= S_APPEND;
	if (flags & HDD_IMMUTABLE_FL)
		inode->i_flags |= S_IMMUTABLE;
	if (flags & HDD_NOATIME_FL)
		inode->i_flags |= S_NOATIME;
	if (flags & HDD_DIRSYNC_FL)
		inode->i_flags |= S_DIRSYNC;
}

/* 读取 inode 对象标志 */
void hdd_get_inode_flags(struct hdd_inode_info *hi)
{
	unsigned int flags = hi->vfs_inode.i_flags;

	hi->i_flags &= ~(HDD_SYNC_FL|HDD_APPEND_FL|HDD_IMMUTABLE_FL|
		      HDD_NOATIME_FL|HDD_DIRSYNC_FL);

	if (flags & S_SYNC)
		hi->i_flags |= HDD_SYNC_FL;
	if (flags & S_APPEND)
		hi->i_flags |= HDD_APPEND_FL;
	if (flags & S_IMMUTABLE)
		hi->i_flags |= HDD_IMMUTABLE_FL;
	if (flags & S_NOATIME)
		hi->i_flags |= HDD_NOATIME_FL;
	if (flags & S_DIRSYNC)
		hi->i_flags |= HDD_DIRSYNC_FL;
}

/* 根据 sb 和 inode 编号, 取得相应 inode 对象 */
struct inode *hdd_iget (struct super_block *sb, unsigned long ino)
{
	struct hdd_sb_info *sbi = HDD_SB(sb);
	struct hdd_inode_info *hi;
	struct buffer_head * bh;
	struct hdd_inode *raw_inode;
	struct inode *inode;
	long ret = -EIO;
	int n;

	inode = iget_locked(sb, ino);	/* 分配 inode 对象和私有信息 */
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))	/* 直接返回旧对象 */
		return inode;

	hi = HDD_I(inode);

	raw_inode = hdd_get_inode(inode->i_sb, ino, &bh);/* 读取磁盘 inode */
	if (IS_ERR(raw_inode)) {
		ret = PTR_ERR(raw_inode);
 		goto bad_inode;
	}

	inode->i_mode = le16_to_cpu(raw_inode->i_mode);
	inode->i_uid = (uid_t)le32_to_cpu(raw_inode->i_uid);
	inode->i_gid = (gid_t)le32_to_cpu(raw_inode->i_gid);
	inode->i_nlink = le16_to_cpu(raw_inode->i_links_count);
	inode->i_size = le64_to_cpu(raw_inode->i_size);
	inode->i_blocks = le32_to_cpu(raw_inode->i_blocks);
	inode->i_atime.tv_sec = (signed)le32_to_cpu(raw_inode->i_atime);
	inode->i_ctime.tv_sec = (signed)le32_to_cpu(raw_inode->i_ctime);
	inode->i_mtime.tv_sec = (signed)le32_to_cpu(raw_inode->i_mtime);
	inode->i_atime.tv_nsec = inode->i_mtime.tv_nsec = inode->i_ctime.tv_nsec = 0;
	inode->i_generation = 0;

	hi->i_dtime = le32_to_cpu(raw_inode->i_dtime);

	if (inode->i_nlink == 0 && (inode->i_mode == 0 || hi->i_dtime)) {
		/* 此 inode 已被删除 */
		brelse (bh);
		ret = -ESTALE;
		goto bad_inode;
	}

	for (n = 0; n < HDD_N_BLOCKS; n++)
		hi->i_data[n] = raw_inode->u.s_hdd.i_block[n];
	hi->i_flags = le32_to_cpu(raw_inode->i_flags);
	hi->i_state = 0;
	hi->i_dtime = 0;
	hi->i_block_group = ino / sbi->inodes_per_group;
	hi->i_dir_start_lookup = 0;
	hi->i_access_count = le32_to_cpu(raw_inode->i_access_count);
	hi->i_ssd_blocks = le32_to_cpu(raw_inode->u.s_hdd.i_ssd_blocks);
	hi->i_direct_bits = raw_inode->u.s_hdd.i_direct_bits;/* 前12个块的标志 */
	for (n = 0; n < HDD_NDIR_BLOCKS; ++n)/* 前12个块的访问计数 */
		hi->i_direct_blks[n] = raw_inode->u.s_hdd.i_direct_blks[n];

	/* 设置 i_op, i_fop, i_mapping 函数表 */
	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &hdd_file_inode_operations;
		inode->i_mapping->a_ops = &hdd_aops;
		inode->i_fop = &hdd_file_operations;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &hdd_dir_inode_operations;
		inode->i_mapping->a_ops = &hdd_aops;
		inode->i_fop = &hdd_dir_operations;
	} else if (S_ISLNK(inode->i_mode)) {
		if (inode->i_blocks == 0) {
			inode->i_op = &hdd_fast_symlink_inode_operations;
			nd_terminate_link(hi->i_data, inode->i_size,
				sizeof(hi->i_data) - 1);/* 确认快链长度 */
		} else {
			inode->i_op = &hdd_symlink_inode_operations;
			inode->i_mapping->a_ops = &hdd_aops;
		}
	} else if (S_ISCHR(inode->i_mode)  || S_ISBLK(inode->i_mode) ||
		   S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
		inode->i_op = &hdd_special_inode_operations;
		init_special_inode(inode, inode->i_mode, 
			new_decode_dev(le32_to_cpu(raw_inode->u.s_hdd.i_block[1])));
	} else {
		ret = -EIO;
		goto bad_inode;
	}

	brelse (bh);
	hdd_set_inode_flags(inode);
	unlock_new_inode(inode);
	return inode;
	
bad_inode:
	iget_failed(inode);
	return ERR_PTR(ret);
}

/* 写 inode 到磁盘 */
int hdd_write_inode(struct inode *inode, int do_sync)
{
	struct hdd_inode_info *hi = HDD_I(inode);
	struct super_block *sb = inode->i_sb;
	ino_t ino = inode->i_ino;
	struct buffer_head * bh;
	struct hdd_inode * raw = hdd_get_inode(sb, ino, &bh);
	int n;
	int err = 0;

	if (IS_ERR(raw))
 		return -EIO;

	if (hi->i_state & HDD_STATE_NEW)	/* 新 inode */
		memset(raw, 0, HDD_INODE_SIZE);

	hdd_get_inode_flags(hi);

	raw->i_mode = cpu_to_le16(inode->i_mode);
	raw->i_links_count = cpu_to_le16(inode->i_nlink);
	raw->i_flags = cpu_to_le32(hi->i_flags);
	raw->i_uid = cpu_to_le32(inode->i_uid);
	raw->i_gid = cpu_to_le32(inode->i_gid);
	raw->i_size = cpu_to_le32(inode->i_size);
	raw->i_blocks = cpu_to_le32(inode->i_blocks);
	raw->i_access_count = cpu_to_le64(hi->i_access_count);
	raw->i_atime = cpu_to_le32(inode->i_atime.tv_sec);
	raw->i_ctime = cpu_to_le32(inode->i_ctime.tv_sec);
	raw->i_mtime = cpu_to_le32(inode->i_mtime.tv_sec);
	raw->i_dtime = cpu_to_le32(hi->i_dtime);

	raw->u.s_hdd.i_ssd_blocks = cpu_to_le32(hi->i_ssd_blocks);
	raw->u.s_hdd.i_direct_bits = hi->i_direct_bits;/* 前12个块的标志 */
	for (n = 0; n < HDD_NDIR_BLOCKS; ++n)/* 前12个块的访问计数 */
		raw->u.s_hdd.i_direct_blks[n] = hi->i_direct_blks[n];

	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		raw->u.s_hdd.i_block[0] = 0;
		raw->u.s_hdd.i_block[1] =
			cpu_to_le32(new_encode_dev(inode->i_rdev));
		raw->u.s_hdd.i_block[2] = 0;
	} else for (n = 0; n < HDD_N_BLOCKS; n++)
		raw->u.s_hdd.i_block[n] = hi->i_data[n];

	mark_buffer_dirty(bh);
	if (do_sync) {
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh)) {
			printk ("IO error syncing hdd inode [%s:%08lx]\n",
				sb->s_id, (unsigned long) ino);
			err = -EIO;
		}
	}

	hi->i_state &= ~HDD_STATE_NEW;
	brelse (bh);
	return err;
}

/* 取得通往块号 i_block 的相对路径, 返回路径深度 */
static int hdd_block_to_path(struct inode *inode,
	long i_block, int offsets[4], int *boundary)
{
	int ptrs = HDD_ADDR_PER_BLOCK;
	const long direct_blocks = HDD_NDIR_BLOCKS,/* 12 直接块 */
		indirect_blocks = ptrs, /* 798 一次间址块 */
		double_blocks = HDD_ADDR_PER_BLOCK << 10;/* 两次间址块数 */
	int n = 0;
	int tmp = 0;
	int bound = 0;

	if (i_block < 0) {
		hdd_msg (inode->i_sb, KERN_WARNING,
			"hdd_block_to_path", "block < 0");
	} else if (i_block < direct_blocks) {
		offsets[n++] = i_block;
		bound = direct_blocks - 1 - i_block;
	} else if ( (i_block -= direct_blocks) < indirect_blocks) {
		offsets[n++] = HDD_IND_BLOCK;
		offsets[n++] = i_block;
		bound = ptrs - 1  - i_block;
	} else if ((i_block -= indirect_blocks) < double_blocks) {
		offsets[n++] = HDD_DIND_BLOCK;
		offsets[n++] = i_block / ptrs;
		offsets[n]   = i_block - offsets[n-1] * ptrs ;
		bound = ptrs - 1 - offsets[n];
		n++;
	} else if ((i_block -= double_blocks) < (ptrs << 20)) {
		offsets[0] = HDD_TIND_BLOCK;
		tmp = i_block / ptrs;
		offsets[1] = tmp >> 10;
		offsets[2] = tmp & (( 1<<10) -1);
		offsets[3] = i_block - tmp * ptrs;
		bound = ptrs - 1 - offsets[3];
		n = 4;
	} else {
		hdd_msg (inode->i_sb, KERN_WARNING,
			"hdd_block_to_path", "block > big");
	}

	if(n >= 2) /* 间接地址的最后一链需要添加偏移量 */
		offsets[n-1] += HDD_ACCESS_END;

	if (boundary)
		/* 位于最后一级地址块的最后一个地址时, 赋值 0, 否则 > 0 */
		*boundary = bound;

	return n;
}

/* 记录块号, 和对应的块内容(地址) */
static inline void add_chain(Indirect *p, struct buffer_head *bh, __le32 *v)
{
	p->key = *(p->p = v); /* p->p  指向地址元素, key 中为地址值 */
	p->bh = bh;
}

/* 验证块号未变 */
static inline int verify_chain(Indirect *from, Indirect *to)
{
	while (from <= to && from->key == *from->p)
		from++;
	return (from > to);
}

/* 跟踪相对路径 offset, 得到实际地址块号路径 chain */
static Indirect * hdd_get_branch(struct inode *inode,
	int depth, int *offsets, Indirect *chain, int *err)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	Indirect *p = chain;

	*err = 0;

	add_chain (p, NULL, HDD_I(inode)->i_data + *offsets);
	if (!p->key) /* 块未分配 */
		return p;

	while (--depth) {
		bh = sb_bread(sb, le32_to_cpu(p->key)); /* 读取元数据块 */
		if (!bh) {
			*err = -EIO;
			return p;
		}

		read_lock(&HDD_I(inode)->i_meta_lock);
		if (!verify_chain(chain, p))
			goto diff;//changed;

		add_chain (++p, bh, (__le32*)bh->b_data + *++offsets);

		read_unlock(&HDD_I(inode)->i_meta_lock);

		if (!p->key)
			return p;
	}
	return NULL;

diff:
	read_unlock(&HDD_I(inode)->i_meta_lock);
	brelse(bh);
	*err = -EAGAIN;
	return p;
}

/* 确定为 inode 中的块号 block 分配块时, 从何处开始查找 */
static unsigned int hdd_find_goal(struct inode *inode,
	long block, Indirect *partial, Indirect *chain, int depth)
{
	/* 将要分配的块在逻辑上紧邻上次分配的块.
	   对于新块的逻辑位置和上一次分配不是紧邻的块，需要查找适当的块。
	   根据具体情况，找到一个尽可能接近间接块的块，或至少在同一柱面组中。
	*/
	struct hdd_inode_info *hi = HDD_I(inode);
	struct hdd_sb_info *sbi = HDD_SB(inode->i_sb);
	Indirect *ind = partial;
	__le32 *start = NULL;
	__le32 *p = NULL;
	unsigned int bg_start = 0;
	unsigned int colour = 0;

	if (!ind->bh) /* 没读取缓冲块, 即为直接块 */
		start = hi->i_data;
	else {
		if(partial - chain == depth - 1)/* bh 中为最后一链中的块 */
			start = (__le32 *) (ind->bh->b_data + HDD_ACCESS_END);
		else /* bh 中为间接链中的块 */
			start = (__le32 *) ind->bh->b_data;
	}
	

	/* 查找前一个数据块的块号 */
	for (p = ind->p - 1; p >= start; p--)
		if (*p)
			return le32_to_cpu(*p);/* 存在, 则从此块号向后找空位 */

	/* 若缓冲块存在(文件洞, 此间接组中没有数据),
	   则从缓冲块的块号开始向后查找空位 */
	if (ind->bh)
		return ind->bh->b_blocknr;

	/* 目标块是 inode 分配的第一个数据块, 在同块组中以随机位置开始查找 */
	bg_start = hdd_group_first_block_no(inode->i_sb, hi->i_block_group);
	colour = (current->pid % 16) * (HDD_BLOCK_SIZE >> 4);
	return bg_start + sbi->grp_data_offset + colour;
}

/* 计算给定分支需要分配的总块数 */
static int hdd_blks_to_allocate(Indirect * branch, int ind_blks,
	unsigned long blks/*1*/, int blocks_to_boundary)
{
	/* 参数 ind_blks 表示需要分配的间接块数 */
	unsigned long count = 0;

	/* 需要分配间接块 */
	if (ind_blks > 0) {
		/* right now don't hanel cross boundary allocation */
		if (blks < blocks_to_boundary + 1)
			count += blks;
		else
			count += blocks_to_boundary + 1;
		return count;
	}

	count++;
	while (count < blks && count <= blocks_to_boundary
		&& le32_to_cpu(*(branch[0].p + count)) == 0) {
		count++;
	}

	return count;
}

/* 分配间接块和直接块 */
static int hdd_alloc_blocks(struct inode *inode,
	unsigned int goal, int indirect_blks, int blks,
	unsigned int new_blocks[4], int *err)
{
/* @indirect_blks: 需要分配的间接块数
 * @new_blocks: 保存分配的间接块和直接块的新块号
 * 返回分配的直接块的块数 */

	int target, i;
	unsigned long count = 0;
	int index = 0;
	unsigned int current_block = 0;
	int ret = 0;

	/* 总块数 = 直接块数 + 间接块数 */
	target = blks + indirect_blks;

	/* 多次调用 hdd_new_blocks, 直到分配了所需的块数 */
	while (1) {
		count = target;
		/* count 保存分配的块数, 返回起始块号 */
		current_block = hdd_new_blocks(inode,goal,&count,err);
		if (*err)
			goto failed_out;

		target -= count; /* 剩余块数 */

		while (index < indirect_blks && count) {/* 分配间接块 */
			new_blocks[index++] = current_block++;
			count--;
		}

		if (count > 0) /* 间接块分配够了 */
			break;
	}

	new_blocks[index] = current_block; /* 记录直接块的块号 */
	
	ret = count; /* 返回分配的直接块数 */
	*err = 0;
	return ret;

failed_out:
	for (i = 0; i <index; i++) /* 释放之前分配的块 */
		hdd_free_blocks(inode, new_blocks[i], 1);
	return ret;
}

/* 对实际路径进行分配, 建立块间的链接, 清除位置 bit, 访问计数 */
static int hdd_alloc_branch(struct inode *inode, int indirect_blks,
	int *blks, unsigned int goal, int *offsets, Indirect *branch)
{
	int i, n = 0;
	int err = 0;
	int num = 0;
	unsigned int  new_blocks[4] = {0};
	unsigned int  current_block = 0;
	struct buffer_head *bh = NULL;
	unsigned int  addr_offset[4] = {0};
	//unsigned int from_direct = 0;
	int blocksize = inode->i_sb->s_blocksize;
	struct hdd_sb_info *sbi = HDD_SB(inode->i_sb);

	/* 分配间接块和直接块, 返回直接块数 */
	num = hdd_alloc_blocks(inode, goal, indirect_blks,
				*blks, new_blocks, &err);
	if (err)
		return err;

	addr_offset[indirect_blks] = HDD_ACCESS_END;/* 调整最后一链的偏移 */
	branch[0].key = cpu_to_le32(new_blocks[0]); /* 新分配的第一个块 */

	/* 先记录间接块链接 */
	for (n = 1; n <= indirect_blks;  n++) { /* 读取父块, 填入其中, 然后写回父块 */
		bh = sb_getblk(inode->i_sb, new_blocks[n-1]);
		branch[n].bh = bh; /* 记录新分配的父块 */

		lock_buffer(bh);
		memset(bh->b_data, 0, blocksize); /* 清零新父块 */
		branch[n].p = (__le32 *) (bh->b_data + addr_offset[n])
			+ offsets[n]; /* 在父块内的地址 */
		branch[n].key = cpu_to_le32(new_blocks[n]);/* 块号 */
		*branch[n].p = branch[n].key;/* 把块号写入父块内 */

		if ( n == indirect_blks) { /* 最后一个链接 */
			current_block = new_blocks[n];

			/* 设置初始访问级别 */
			access_info_init(inode, bh, offsets[n]);

			/* 使最后一链指向新分配的多个有效数据块 */
			for (i=1; i < num; i++) {
				access_info_init(inode, bh, offsets[n] + i);
				*(branch[n].p + i) = cpu_to_le32(++current_block);
			}
		}

		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		mark_buffer_dirty_inode(bh, inode);
		if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode))
			sync_dirty_buffer(bh);
	}

	if (indirect_blks == 0) { /* 只分配了数据块 */
		*branch[0].p = branch[0].key; /* 记录到地址表中 */
		if (NULL == branch[0].bh )/* 数据块由直接块指向 */
			access_info_init(inode, NULL, offsets[0]);
		else { /* 数据块由间接块指向 */
			lock_buffer(branch[0].bh);
			/* 设置新块的访问计数 */
			access_info_init(inode, branch[0].bh, offsets[0]);

			set_buffer_uptodate(bh);
			unlock_buffer(bh);
			mark_buffer_dirty_inode(bh, inode);
			if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode))
				sync_dirty_buffer(bh);
		}
	}

	*blks = num;

	down_read(&sbi->sbi_rwsem);
	percpu_counter_inc(&sbi->usr_blocks); /* 增加用户纯数据的块数 */
	up_read(&sbi->sbi_rwsem);

	inode->i_ctime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode);	/* 标记 inode 更改 */

	return err;
}

/* 读取文件中相对块号 iblock 的实际块号, 记录到 bh_result 中, 若不存在则分配之 */
static int hdd_get_blocks(struct inode *inode,
	sector_t iblock, unsigned long maxblocks,
	struct buffer_head *bh_result, int create)
{
	int err = -EIO;
	int offsets[4] = {0};		/* 相对块号路径 */
	Indirect chain[4];		/* 实际块号路径 */
	int depth = 0;			/* 路径深度 */
	Indirect *partial = NULL;	/* 实际路径中需要分配的起点 */
	unsigned int goal = 0;		/* 建议目标 */
	int indirect_blks = 0;		/* 需要分配的间接块号 */
	int count = 0;			/* 记录的直接块数 */
	int blocks_to_boundary = 0;	/* 距离一级间接块结尾的地址个数 */
	int location = 0;		/* 块位置: 1-SSD, 0-HDD */
	struct hdd_inode_info *hi = HDD_I(inode);
	struct hdd_sb_info *sbi = HDD_SB(inode->i_sb);

	/* 根据数据块在文件中的位置,找到通向块的路径-最长三间 */
	depth = hdd_block_to_path(inode, iblock,
				  offsets, &blocks_to_boundary);
	if (depth == 0)
		return (err);

	/* 跟踪相对路径, 得到到达数据块的实际路径 */
	partial = hdd_get_branch(inode, depth, offsets, chain, &err);

	/* 不需要分配数据块的情况 */
	if (!partial) {
		/* 标记 buffer 不是最新的, 需要从设备读取 */
		clear_buffer_new (bh_result);
		count++;
		if (err != -EAGAIN)
			goto got_it;  /* 跳过 */
	}

	/* 需要分配数据的情况 */
	if (!create || err == -EIO)/* 不允许分配新块, 或者读取失败 */
		goto cleanup;

	mutex_lock(&hi->truncate_mutex);
	if (err == -EAGAIN || !verify_chain(chain, partial)) {
		while (partial > chain) {
			brelse(partial->bh);
			partial--;
		}
		/* 跟踪相对路径, 得到实际路径 */
		partial = hdd_get_branch(inode, depth, offsets, chain, &err);
		if (!partial) {
			count++;
			mutex_unlock(&hi->truncate_mutex);
			if (err)
				goto cleanup;
			clear_buffer_new(bh_result);
			goto got_it; /* 跳过 */
		}
	}

	/* 需要分配块的情况 */
	goal = hdd_find_goal(inode, iblock, 
		partial, chain, depth);/* 查找空位的起始块号 */

	/* 计算需要分配的间接块数 */
	indirect_blks = (chain + depth) - partial - 1;

	/* count 为需要分配的直接块数: 1 */
	count = hdd_blks_to_allocate(partial, indirect_blks,
				maxblocks, blocks_to_boundary);
	/* 读实际路径进行分配 */
	err = hdd_alloc_branch(inode, indirect_blks, &count, goal,
				offsets + (partial - chain), partial);
	mutex_unlock(&hi->truncate_mutex);

	if (err) 
		goto cleanup;

	set_buffer_new(bh_result);
got_it:
	/* 根据 inode 中的地址块的相对块号, 更新访问计数, 并返回所在位置:SSD/HDD */
	location = access_info_inc(inode, chain+depth-1, offsets[depth-1]);

	/* 根据实际位置, 则可记录 [设备+实际块号] 到 bh_result 中 */
	if(location == BLOCK_ON_SSD) {
		set_buffer_mapped(bh_result);
		bh_result->b_bdev = sbi->ssd_bdev;
		bh_result->b_blocknr = le32_to_cpu(chain[depth-1].key);
		bh_result->b_size = inode->i_sb->s_blocksize;
	} else {/* 未迁移 */
		map_bh(bh_result, inode->i_sb, le32_to_cpu(chain[depth-1].key));
	}

	/* 记录缓冲块是否为一个链接中的最后一个块: 如 11, 797 等 */
	if (location == BLOCK_ON_HDD && count > blocks_to_boundary)
		set_buffer_boundary(bh_result);
	err = count; /* 返回值为读取的直接块数 */

	partial = chain + depth - 1;	/* the whole chain */
cleanup:
	while (partial > chain) { /* 释放路径上的间接数据块 */
		brelse(partial->bh);
		partial--;
	}
	return err;
}

/* 读取相对块号为 iblock 的块的实际块号, 记录到 bh 中 */
int hdd_get_block(struct inode *inode, sector_t iblock, 
	struct buffer_head *bh_result, int create)
{
	/* 读取的块数 : 1 */
	unsigned max_blocks = bh_result->b_size >> inode->i_blkbits;
	/* 开始读取, 返回值为读取的块数 */
	int ret/*1*/ = hdd_get_blocks(inode, iblock, max_blocks,/*1*/
		bh_result, create);

	if (ret > 0) {
		bh_result->b_size = (ret << inode->i_blkbits);
		ret = 0;
	}

	return ret;/* 0 为成功 */
}

/* 释放一些直接数据块, 范围为 [p,q) */
static inline void hdd_free_data(struct inode *inode, __le32 *p, __le32 *q)
{
	unsigned long block_to_free = 0, count = 0;
	unsigned long nr = 0; /* 块号 */
	unsigned int freed = 0;
	struct hdd_sb_info *sbi = HDD_SB(inode->i_sb);

	for ( ; p < q ; p++) {
		nr = le32_to_cpu(*p);
		if (nr) {/*若块号不为0, 则在HDD上, 否则块号不存在,或在SSD上被释放 */
			*p = 0;
			/* accumulate blocks to free if they're contiguous */
			if (count == 0)
				goto free_this;
			else if (block_to_free == nr - count)
				count++;
			else {
				mark_inode_dirty(inode);
				/* 在块位图中释放从 block_to_free 开始的 count 个块 */
				hdd_free_blocks (inode, block_to_free, count);
				freed += count;
			free_this:
				block_to_free = nr;
				count = 1;
			}
		}
	}

	if (count > 0) {
		mark_inode_dirty(inode);
		/* 在块位图中释放从 block_to_free 开始的 count 个块 */
		hdd_free_blocks (inode, block_to_free, count);
		freed += count;
	}

	down_read(&sbi->sbi_rwsem);
	percpu_counter_sub(&sbi->usr_blocks, freed);
	up_read(&sbi->sbi_rwsem);	
}

/* 检查[p, q) 范围内是否全为 0 */
static inline int all_zeroes(__le32 *p, __le32 *q)
{
	while (p < q)
		if (*p++)
			return 0;
	return 1;
}

/* 查找 partial truancate 的间接块 */
static Indirect *hdd_find_shared(struct inode *inode, int depth,
	int offsets[4], Indirect chain[4], __le32 *top)
{
/* 共享: 指待清除块和保留块共享一个分支
 * @chain:  place to store the pointers to partial indirect blocks
 * @top:    place to the (detached) top of branch
 *
 * 当 truncate() 时, 可能需要在清除间接块的尾部时,使间接块自己有效. 
 * 如果一些低于 i_size 的数据块引用了间接块,则间接块是被部分截断的.
 * 则需要释放这个路径上 top 至 最右端的块. 在 truncate()结束之前, 没有数据分配会越过
 * truncation point. 但分支的 top 需要特别注意, 因为低部分数据页可能会使用它.
 */
	Indirect *partial = NULL; /* 指向最外层路径 */
	Indirect *p = NULL;/* 指向最远的共享路径 */
	int k = 0;
	int err = 0;
	int addr_offset = 0;

	*top = 0;
	/* 从后向前,找到不是一链中的首个块的链 */
	for (k = depth; k > 1 && !offsets[k-1]; k--)
		;
	/* 取得共享路径 */
	partial = hdd_get_branch(inode, k, offsets, chain, &err);

	/* 1.如果所有块都已分配, 则最远共享路径为最外层 */
	if (!partial)
		partial = chain + k-1;

	write_lock(&HDD_I(inode)->i_meta_lock);

	/* 2.如果partial 指向的块并未分配, 则返回 partial, 且 *top 为0 */
	if (!partial->key && *partial->p) {
		write_unlock(&HDD_I(inode)->i_meta_lock);
		goto no_top; /* 直接去释放共享间接块 */
	}

	/* 首相把共享路径指向最外层路径 */
	p = partial;
	/* 若同一链中 p->p 之前的块号都为 0, 即都未分配, 则前移 p, 缩短共享路径 */
	while(1) {
		if (p + 1 - chain == depth)
			addr_offset = HDD_ACCESS_END;
		else
			addr_offset = 0;

		if (p > chain
		&& all_zeroes((__le32*)(p->bh->b_data+addr_offset),p->p)) 
			p--;
		else 
			break;
	}

	if (p == chain + k - 1 && p > chain) {
		/* 若最远共享地址块是最外层且不是第一层, 则没有 top, 
		并调整 p 指向最后一个有效地址, 使得释放共享地址块时可以 + 1 处开始释放 */
		p->p--; 
	} else {/* 若最远共享地址块不是最外层, 或是第一层 */
		*top = *p->p;/* 则记录外层待释放块号, 单独处理之 */
		*p->p = 0;/* 然后释放此地址, 使得之后释放共享地址块时, 从下一个块开始释放 */
	}

	write_unlock(&HDD_I(inode)->i_meta_lock);

	while(partial > p) {	/* 释放 p 之后的缓冲块 */
		brelse(partial->bh);
		partial--;
	}
no_top:
	return partial /* p */;
}

/* 释放一组分支 [p,q) */
static void hdd_free_branches(struct inode *inode,
	__le32 *p, __le32 *q, int depth, char *data)
{
/* @p: 保存根块号的数组
 * @q: 数组末端后的位置
 * @depth: 元素表示的分支的深度 - 1, 2, 3
 * data: 地址块的地址, 即如 bh->b_data
 */
	struct buffer_head * bh = NULL;
	unsigned long nr = 0;

	if (depth--) {/* 释放中间链地址块 */
		int addr_offset = 0;
		if (0 == depth) 
			addr_offset = HDD_ACCESS_END;

		for ( ; p < q ; p++) {
			nr = le32_to_cpu(*p); /* 释放的块号 */
			if (!nr) /* 块号为 0, 没有数据, 释放下一块号 */
				continue; 
			*p = 0; /* 清除此链路 */

			bh = sb_bread(inode->i_sb, nr); /* 取得地址块内容 */
			if (!bh) {
				hdd_msg(inode->i_sb, KERN_ERR,"hdd_free_branches",
					"Read failure, inode=%ld, block=%ld",
					inode->i_ino, nr);
				continue;
			}

			hdd_free_branches(inode, /* 释放块中所有地址 */
					  (__le32*)(bh->b_data + addr_offset),
					  (__le32*)bh->b_data + 1024,
					  depth,
					  bh->b_data);
			bforget(bh); /* 无需再写回释放的中间链的块 */
			hdd_free_blocks(inode, nr, 1); /* 释放块回块位图中 */
			mark_inode_dirty(inode);
		}
	} else {/* 释放最后一链地址块 */
		/* 释放访问信息和 SSD 上的块: 
		inode, 地址块起始地址, 首个释放块地址偏移, 释放的块数 */
		access_info_sub(inode, (__le32 *)data, p - (__le32 *) data, q - p);

		hdd_free_data(inode, p, q);
	}
}

/* 释放磁盘数据块, 到 inode 中指定的大小 */
void hdd_truncate(struct inode *inode)
{
	__le32 *i_data = HDD_I(inode)->i_data;
	struct hdd_inode_info *hi = HDD_I(inode);
	long iblock;		/* 相对块号 */
	unsigned blocksize;	/* 块长 */
	int offsets[4];		/* 相对块号路径 */
	int n;			/* 路径深度 */
	Indirect chain[4];	/* 实际块号路径 */
	Indirect *partial;	/* 实际路径中的处理起点 */
	__le32 nr = 0;		/* 块号 */
	//__le32 *p = NULL;	/* 块号在地址块中的地址 */
	//__le32 **addr = &p;

	/* 只处理常规,目录,非快速符号链接 */
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	    S_ISLNK(inode->i_mode)))
		return;
	if (S_ISLNK(inode->i_mode) && inode->i_blocks == 0)//
		return;
	/* 非只添加文件, 非不可更改文件 */
	if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
		return;

	/* 块长和要截断的起始相对块号 */
	blocksize = inode->i_sb->s_blocksize;
	iblock = (inode->i_size + blocksize-1) >> HDD_BLOCK_LOG_SIZE;

	/* 把 inode 中的数据修改为 i_size 大小:路径不存在则分配,+ 微调 */
	block_truncate_page(inode->i_mapping,
			inode->i_size, hdd_get_block);

	/* 取得 iblock 的相对路径, 返回深度 */
	n = hdd_block_to_path(inode, iblock, offsets, NULL);
	if (n == 0)
		return;

	mutex_lock(&hi->truncate_mutex);

	/* 若截断起始块在直接块内, 则先释放多余直接块 */
	if (n == 1) {
		/* 释放位置和访问信息 */
		access_info_sub(inode, i_data, offsets[0], 
			HDD_NDIR_BLOCKS - offsets[0]);
		/* 释放数据块 */
		hdd_free_data(inode, i_data+offsets[0],
				i_data + HDD_NDIR_BLOCKS);
		goto do_indirects; /* 再去释放所有间接块 */
	}

	/* 取得共享路径 chain, 
	   并返回最外层的同时包含待释放块和保留块的地址块 (此时, n > 1) */
	partial = hdd_find_shared(inode, n, offsets, chain, &nr);

	/* nr 记录了要释放的块,要释放此块中所有地址
	Kill the top of shared branch (already detached) */
	if (nr) {
		if (partial == chain) {
			/* top 为地址表中的间接地址 (i_data 中值已被置 0) */
			mark_inode_dirty(inode);
			hdd_free_branches(inode, &nr, &nr+1, n-1, NULL);
		} else {/* 释放中间链中的位置 */
			mark_buffer_dirty_inode(partial->bh, inode);
			/* 若 nr != 0, 且 partial != chain, 则 partial 必 < chain+n-1,
			   即 (chain+n-1) - partial > 0 */
			hdd_free_branches(inode, &nr, &nr+1, 
				(chain+n-1) - partial, NULL);
		}
	}

	/* 释放分支中的共享间接块中的地址 */
	while (partial > chain) {/* partial 一定指向直接链 */
		hdd_free_branches(inode,
				  partial->p + 1,/* +1 */
				  (__le32*)partial->bh->b_data + 1024,
				  (chain+n-1) - partial, partial->bh->b_data);

		mark_buffer_dirty_inode(partial->bh, inode);
		brelse (partial->bh);
		partial--;
	}

do_indirects:
	/* 释放剩余的非共享分支地址树 */
	switch (offsets[0]) {
		default: /* 若 truncate 直接块 */
			nr = i_data[HDD_IND_BLOCK];/* 则释放一间链 */
			if (nr) {
				i_data[HDD_IND_BLOCK] = 0;
				mark_inode_dirty(inode);
				hdd_free_branches(inode, &nr, &nr+1, 1, NULL);
			}
		case HDD_IND_BLOCK: /* 一次间址 */
			nr = i_data[HDD_DIND_BLOCK];/* 然后释放二间链 */
			if (nr) {
				i_data[HDD_DIND_BLOCK] = 0;
				mark_inode_dirty(inode);
				hdd_free_branches(inode, &nr, &nr+1, 2, NULL);
			}
		case HDD_DIND_BLOCK:/* 二次间址 */
			nr = i_data[HDD_TIND_BLOCK];/* 释放三间链 */
			if (nr) {
				i_data[HDD_TIND_BLOCK] = 0;
				mark_inode_dirty(inode);
				hdd_free_branches(inode, &nr, &nr+1, 3, NULL);
			}
		case HDD_TIND_BLOCK:
			;
	}

	mutex_unlock(&hi->truncate_mutex);

	inode->i_mtime = inode->i_ctime = CURRENT_TIME_SEC;

	if (inode_needs_sync(inode)) {
		sync_mapping_buffers(inode->i_mapping);
		hdd_sync_inode (inode);
	} else {
		mark_inode_dirty(inode);
	}
}

/* 删除磁盘上的 inode */
void hdd_delete_inode(struct inode *inode)
{
	truncate_inode_pages(&inode->i_data, 0);

	if (is_bad_inode(inode)) {
		clear_inode(inode);
		return;
	}

	HDD_I(inode)->i_dtime	= get_seconds();

	mark_inode_dirty(inode);
	hdd_write_inode(inode, inode_needs_sync(inode));

	inode->i_size = 0;
	if (inode->i_blocks)
		hdd_truncate (inode); /* 释放 inode 的磁盘数据块 */

	hdd_free_inode (inode); /* 释放 inode 本身 */
}

/* 同步 inode */
int hdd_sync_inode(struct inode *inode)
{
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = 0,	/* sys_fsync did this */
	};
	return sync_inode(inode, &wbc);
}

/* 初始化访问信息 */
void access_info_init(struct inode *inode,struct buffer_head *bh, unsigned int offset)
{
/*
  调用者持有 inode->truncate_lock,
  在调用后标记 bh 和 inode 为脏.
 
  bh 若为 NULL, 表示 offset 为直接块偏移, 否则为间接块偏移.
 */
}

/* 增加访问计数, 若访问计数超过了平均值, 则迁移到 SSD, 返回位置 */
int access_info_inc(struct inode * inode, Indirect *branch, unsigned int offset)
{
/*
  branch.bh 若为 NULL, 表示 offset 为直接块偏移, 否则为间接块偏移.
  
  
  //若是目录文件, 则一定进行迁移;

  若原来在HDD上, 则更新SSD上的访问信息, 访问级别等, 并判断是否需要做迁移
	  branch->key 记录了块号, 若在此新迁移, 则需要更新其值.并标记 bh 为脏.

  若原来在SSD上, 则更新SSD上的访问信息,

  返回 1 表示在SSD上, 0 表示在 HDD 上
 */
	//struct buffer_head *bh;
	return 0;
}

/* 释放数据块, 减少其访问计数, 若块在SSD上, 则释放块, 并标记块号为 0 */
void access_info_sub(struct inode *inode, __le32 *data, int offset, int count)
{
	/* data 为地址块起始地址, 若等于 i_data, 则表示直接块, 否则为地址链中最后一链.
	 * offset 表示要释放的起始块在 data 中的偏移.
	 * count 表示要释放的块数.
	 */


}

/* 设置文件基本属性 */
int hdd_setattr(struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	int error;

	error = inode_change_ok(inode, iattr); /* 检查权限 */
	if (error)
		return error;

	error = inode_setattr(inode, iattr);

	return error;
}

/* 读取一页 */
static int hdd_readpage(struct file *file, struct page *page)
{
	return mpage_readpage(page, hdd_get_block);
}

/* 读取多页 */
static int hdd_readpages(struct file *file, struct address_space *mapping,
	struct list_head *pages, unsigned nr_pages)
{
	return mpage_readpages(mapping, pages, nr_pages, hdd_get_block);
}

/* 写一页 */
static int hdd_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, hdd_get_block, wbc);
}

/* 写多页 */
static int hdd_writepages(struct address_space *mapping, 
	struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, hdd_get_block);
}

int __hdd_write_begin(struct file *file, struct address_space *mapping,
	loff_t pos, unsigned len, unsigned flags,
	struct page **pagep, void **fsdata)
{
	return block_write_begin(file, mapping, pos, 
		len, flags, pagep, fsdata, hdd_get_block);
}

static int hdd_write_begin(struct file *file, struct address_space *mapping,
	loff_t pos, unsigned len, unsigned flags,
	struct page **pagep, void **fsdata)
{
	*pagep = NULL;
	return __hdd_write_begin(file, mapping, pos, len, flags, pagep,fsdata);
}

static ssize_t hdd_direct_IO(int rw, struct kiocb *iocb, 
	const struct iovec *iov, loff_t offset, unsigned long nr_segs)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;

	return blockdev_direct_IO(rw, iocb, inode, inode->i_sb->s_bdev, iov,
		offset, nr_segs, hdd_get_block, NULL);
}

static sector_t hdd_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, hdd_get_block);
}

int hdd_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		u64 start, u64 len)
{
	return generic_block_fiemap(inode, fieinfo, start, len,
		hdd_get_block);
}

/* 关联文件系统和块层 */
const struct address_space_operations hdd_aops = {
	.readpage		= hdd_readpage,
	.readpages		= hdd_readpages,

	.writepage		= hdd_writepage,
	.writepages		= hdd_writepages,

	.sync_page		= block_sync_page,

	.write_begin		= hdd_write_begin,
	.write_end		= generic_write_end,

	.direct_IO		= hdd_direct_IO,
	.bmap			= hdd_bmap,

	//.set_page_dirty		= f2fs_set_data_page_dirty,
	//.invalidatepage		= f2fs_invalidate_data_page,
	//.releasepage		= f2fs_release_data_page,

	.is_partially_uptodate	= block_is_partially_uptodate,
	.error_remove_page	= generic_error_remove_page,
};

