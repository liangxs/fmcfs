/*
 * linux/fs/fmc/hdd_inode.c
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * Beijing University of Posts and Telecommunications,
 * CPU Center @ Tsinghua University.
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
	__le32	*p;		/* ָ��һ����� */
	__le32	key;		/* ������ָ��Ŀ��ֵ */
	struct buffer_head *bh; /* ��Ŷ�Ӧ�Ŀ����� */
} Indirect;

/* �Ӵ��̶�ȡ inode �ṹ */
static struct hdd_inode *hdd_get_inode(struct super_block *sb,
	ino_t ino, struct buffer_head **p)
{
	struct buffer_head * bh;
	unsigned int block_group;
	unsigned int block;
	unsigned int offset;
	struct hdd_group_desc * desc;

	*p = NULL;
	if ((ino != HDD_ROOT_INO && ino < HDD_FIRST_INO) /* ȷ�� ino �Ϸ� */
	||   ino >= HDD_SB(sb)->inodes_count)
		goto Einval;

	block_group = ino / HDD_SB(sb)->inodes_per_group;
	desc = hdd_get_group_desc(sb, block_group, NULL);/* ȡ�������� */
	if (!desc)
		goto Edesc;

	/* inode �ṹ��ƫ���������ڿ�� */
	offset = (ino % HDD_SB(sb)->inodes_per_group) * HDD_INODE_SIZE;
	block = le32_to_cpu(desc->bg_inode_table) +
		(offset >> HDD_BLOCK_LOG_SIZE);

	if (!(bh = sb_bread(sb, block)))	/* ��ȡ inode ���ڿ� */
		goto Eio;

	*p = bh;
	offset &= (HDD_BLOCK_SIZE - 1);		/* �ڿ��е���ʼ��ַ */
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

/* ���� inode ����ı�־ */
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

/* ��ȡ inode �����־ */
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

/* ���� sb �� inode ���, ȡ����Ӧ inode ���� */
struct inode *hdd_iget (struct super_block *sb, unsigned long ino)
{
	struct hdd_sb_info sbi = HDD_SB(sb);
	struct hdd_inode_info *hi;
	struct buffer_head * bh;
	struct hdd_inode *raw_inode;
	struct inode *inode;
	long ret = -EIO;
	int n;

	inode = iget_locked(sb, ino);	/* ���� inode �����˽����Ϣ */
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))	/* ֱ�ӷ��ؾɶ��� */
		return inode;

	hi = HDD_I(inode);

	raw_inode = hdd_get_inode(inode->i_sb, ino, &bh);/* ��ȡ���� inode */
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
		/* �� inode �ѱ�ɾ�� */
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
	hi->i_direct_bits = raw_inode->u.s_hdd.i_direct_bits;/* ǰ12����ı�־ */
	for (n = 0; n < HDD_NDIR_BLOCKS; ++n)/* ǰ12����ķ��ʼ��� */
		hi->i_direct_blks[n] = raw_inode->u.s_hdd.i_direct_blks[n];

	/* ���� i_op, i_fop, i_mapping ������ */
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
				sizeof(hi->i_data) - 1);/* ȷ�Ͽ������� */
		} else {
			inode->i_op = &hdd_symlink_inode_operations;
			inode->i_mapping->a_ops = &hdd_aops;
		}
	} else if (S_ISCHR(inode->i_mode)  || S_ISBLK(inode->i_mode) ||
		   S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
		inode->i_op = &hdd_special_inode_operations;
		init_special_inode(inode, inode->i_mode, 
			new_decode_dev(le32_to_cpu(raw_inode->i_block[1])));
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

/* д inode ������ */
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

	if (hi->i_state & HDD_STATE_NEW)	/* �� inode */
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
	raw->u.s_hdd.i_direct_bits = hi->i_direct_bits;/* ǰ12����ı�־ */
	for (n = 0; n < HDD_NDIR_BLOCKS; ++n)/* ǰ12����ķ��ʼ��� */
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

/* ȡ��ͨ����� i_block �����·��, ����·����� */
static int hdd_block_to_path(struct inode *inode,
	long i_block, int offsets[4], int *boundary)
{
	int ptrs = HDD_ADDR_PER_BLOCK;
	const long direct_blocks = HDD_NDIR_BLOCKS,/* 12 ֱ�ӿ� */
		indirect_blocks = ptrs, /* 798 һ�μ�ַ�� */
		double_blocks = HDD_ADDR_PER_BLOCK << 10;/* ���μ�ַ���� */
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

	if(n >= 2) /* ��ӵ�ַ�����һ����Ҫ����ƫ���� */
		offsets[n-1] += HDD_ACCESS_END;

	if (boundary)
		/* λ�����һ����ַ������һ����ַʱ, ��ֵ 0, ���� > 0 */
		*boundary = bound;

	return n;
}

/* ��¼���, �Ͷ�Ӧ�Ŀ�����(��ַ) */
static inline void add_chain(Indirect *p, struct buffer_head *bh, __le32 *v)
{
	p->key = *(p->p = v); /* p->p  ָ���ַԪ��, key ��Ϊ��ֵַ */
	p->bh = bh;
}

/* ��֤���δ�� */
static inline int verify_chain(Indirect *from, Indirect *to)
{
	while (from <= to && from->key == *from->p)
		from++;
	return (from > to);
}

/* �������·�� offset, �õ�ʵ�ʵ�ַ���·�� chain */
static Indirect * hdd_get_branch(struct inode *inode,
	int depth, int *offsets, Indirect *chain, int *err)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	Indirect *p = chain;

	*err = 0;

	add_chain (p, NULL, HDD_I(inode)->i_data + *offsets);
	if (!p->key) /* ��δ���� */
		return p;

	while (--depth) {
		bh = sb_bread(sb, le32_to_cpu(p->key)); /* ��ȡԪ���ݿ� */
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

/* ȷ��Ϊ inode �еĿ�� block �����ʱ, �Ӻδ���ʼ���� */
static unsigned int hdd_find_goal(struct inode *inode,
	long block, Indirect *partial, Indirect *chain, int depth)
{
	/* ��Ҫ����Ŀ����߼��Ͻ����ϴη���Ŀ�.
	   �����¿���߼�λ�ú���һ�η��䲻�ǽ��ڵĿ飬��Ҫ�����ʵ��Ŀ顣
	   ���ݾ���������ҵ�һ�������ܽӽ���ӿ�Ŀ飬��������ͬһ�������С�
	*/
	struct hdd_inode_info *hi = HDD_I(inode);
	struct hdd_super_block sbi = HDD_SB(inode->i_sb);
	Indirect *ind = partial;
	__le32 *start = NULL;
	__le32 *p = NULL;
	unsigned int bg_start = 0;
	unsigned int colour = 0;

	if (!ind->bh) /* û��ȡ�����, ��Ϊֱ�ӿ� */
		start = hi->i_data;
	else {
		if(partial - chain == depth - 1)/* bh ��Ϊ���һ���еĿ� */
			start = (__le32 *) (ind->bh->b_data + HDD_ACCESS_END): 
		else /* bh ��Ϊ������еĿ� */
			start = (__le32 *) ind->bh->b_data;
	}
	

	/* ����ǰһ�����ݿ�Ŀ�� */
	for (p = ind->p - 1; p >= start; p--)
		if (*p)
			return le32_to_cpu(*p);/* ����, ��Ӵ˿������ҿ�λ */

	/* ����������(�ļ���, �˼������û������),
	   ��ӻ����Ŀ�ſ�ʼ�����ҿ�λ */
	if (ind->bh)
		return ind->bh->b_blocknr;

	/* Ŀ����� inode ����ĵ�һ�����ݿ�, ��ͬ�����������λ�ÿ�ʼ���� */
	bg_start = hdd_group_first_block_no(inode->i_sb, hi->i_block_group);
	colour = (current->pid % 16) * (HDD_BLOCK_SIZE >> 4);
	return bg_start + sbi->grp_data_offset + colour;
}

/* ���������֧��Ҫ������ܿ��� */
static int hdd_blks_to_allocate(Indirect * branch, int ind_blks,
	unsigned long blks/*1*/, int blocks_to_boundary)
{
	/* ���� ind_blks ��ʾ��Ҫ����ļ�ӿ��� */
	unsigned long count = 0;

	/* ��Ҫ�����ӿ� */
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

/* �����ӿ��ֱ�ӿ� */
static int hdd_alloc_blocks(struct inode *inode,
	unsigned int goal, int indirect_blks, int blks,
	unsigned int new_blocks[4], int *err)
{
/* @indirect_blks: ��Ҫ����ļ�ӿ���
 * @new_blocks: �������ļ�ӿ��ֱ�ӿ���¿��
 * ���ط����ֱ�ӿ�Ŀ��� */

	int target, i;
	unsigned long count = 0;
	int index = 0;
	unsigned int current_block = 0;
	int ret = 0;

	/* �ܿ��� = ֱ�ӿ��� + ��ӿ��� */
	target = blks + indirect_blks;

	/* ��ε��� hdd_new_blocks, ֱ������������Ŀ��� */
	while (1) {
		count = target;
		/* count �������Ŀ���, ������ʼ��� */
		current_block = hdd_new_blocks(inode,goal,&count,err);
		if (*err)
			goto failed_out;

		target -= count; /* ʣ����� */

		while (index < indirect_blks && count) {/* �����ӿ� */
			new_blocks[index++] = current_block++;
			count--;
		}

		if (count > 0) /* ��ӿ���乻�� */
			break;
	}

	new_blocks[index] = current_block; /* ��¼ֱ�ӿ�Ŀ�� */
	
	ret = count; /* ���ط����ֱ�ӿ��� */
	*err = 0;
	return ret;

failed_out:
	for (i = 0; i <index; i++) /* �ͷ�֮ǰ����Ŀ� */
		hdd_free_blocks(inode, new_blocks[i], 1);
	return ret;
}

/* ��ʵ��·�����з���, ������������, ���λ�� bit, ���ʼ��� */
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
	unsigned int from_direct = 0;
	int blocksize = inode->i_sb->s_blocksize;
	struct hdd_sb_info sbi = HDD_SB(inode->i_sb);

	/* �����ӿ��ֱ�ӿ�, ����ֱ�ӿ��� */
	num = hdd_alloc_blocks(inode, goal, indirect_blks,
				*blks, new_blocks, &err);
	if (err)
		return err;

	addr_offset[indirect_blks] = HDD_ACCESS_END;/* �������һ����ƫ�� */
	branch[0].key = cpu_to_le32(new_blocks[0]); /* �·���ĵ�һ���� */

	/* �ȼ�¼��ӿ����� */
	for (n = 1; n <= indirect_blks;  n++) { /* ��ȡ����, ��������, Ȼ��д�ظ��� */
		bh = sb_getblk(inode->i_sb, new_blocks[n-1]);
		branch[n].bh = bh; /* ��¼�·���ĸ��� */

		lock_buffer(bh);
		memset(bh->b_data, 0, blocksize); /* �����¸��� */
		branch[n].p = (__le32 *) (bh->b_data + addr_offset[n])
			+ offsets[n]; /* �ڸ����ڵĵ�ַ */
		branch[n].key = cpu_to_le32(new_blocks[n]);/* ��� */
		*branch[n].p = branch[n].key;/* �ѿ��д�븸���� */

		if ( n == indirect_blks) { /* ���һ������ */
			current_block = new_blocks[n];

			/* ���ó�ʼ���ʼ��� */
			access_info_init(inode, bh, offsets[n]);

			/* ʹ���һ��ָ���·���Ķ����Ч���ݿ� */
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

	if (indirect_blks == 0) { /* ֻ���������ݿ� */
		*branch[0].p = branch[0].key; /* ��¼����ַ���� */
		if (NULL == branch[0].bh )/* ���ݿ���ֱ�ӿ�ָ�� */
			access_info_init(inode, NULL, offsets[0]);
		else { /* ���ݿ��ɼ�ӿ�ָ�� */
			lock_buffer(branch[0].bh);
			/* �����¿�ķ��ʼ��� */
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
	percpu_counter_inc(&sbi->usr_blocks); /* �����û������ݵĿ��� */
	up_read(&sbi->sbi_rwsem);

	inode->i_ctime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode);	/* ��� inode ���� */

	return err;
}

/* ��ȡ�ļ�����Կ�� iblock ��ʵ�ʿ��, ��¼�� bh_result ��, �������������֮ */
static int hdd_get_blocks(struct inode *inode,
	sector_t iblock, unsigned long maxblocks,
	struct buffer_head *bh_result, int create)
{
	int err = -EIO;
	int offsets[4] = {0};		/* ��Կ��·�� */
	Indirect chain[4];		/* ʵ�ʿ��·�� */
	int depth = 0;			/* ·����� */
	Indirect *partial = NULL;	/* ʵ��·������Ҫ�������� */
	unsigned int goal = 0;		/* ����Ŀ�� */
	int indirect_blks = 0;		/* ��Ҫ����ļ�ӿ�� */
	int count = 0;			/* ��¼��ֱ�ӿ��� */
	int blocks_to_boundary = 0;	/* ����һ����ӿ��β�ĵ�ַ���� */
	int location = 0;		/* ��λ��: 1-SSD, 0-HDD */
	struct hdd_inode_info *hi = HDD_I(inode);
	struct hdd_sb_info *sbi = HDD_SB(inode->i_sb);

	/* �������ݿ����ļ��е�λ��,�ҵ�ͨ����·��-����� */
	depth = hdd_block_to_path(inode, iblock,
				  offsets, &blocks_to_boundary);
	if (depth == 0)
		return (err);

	/* �������·��, �õ��������ݿ��ʵ��·�� */
	partial = hdd_get_branch(inode, depth, offsets, chain, &err);

	/* ����Ҫ�������ݿ����� */
	if (!partial) {
		/* ��� buffer �������µ�, ��Ҫ���豸��ȡ */
		clear_buffer_new (bh_result);
		count++;
		if (err != -EAGAIN)
			goto got_it;  /* ���� */
	}

	/* ��Ҫ�������ݵ���� */
	if (!create || err == -EIO)/* �����������¿�, ���߶�ȡʧ�� */
		goto cleanup;

	mutex_lock(&hi->truncate_mutex);
	if (err == -EAGAIN || !verify_chain(chain, partial)) {
		while (partial > chain) {
			brelse(partial->bh);
			partial--;
		}
		/* �������·��, �õ�ʵ��·�� */
		partial = hdd_get_branch(inode, depth, offsets, chain, &err);
		if (!partial) {
			count++;
			mutex_unlock(&hi->truncate_mutex);
			if (err)
				goto cleanup;
			clear_buffer_new(bh_result);
			goto got_it; /* ���� */
		}
	}

	/* ��Ҫ��������� */
	goal = hdd_find_goal(inode, iblock, 
		partial, chain, depth);/* ���ҿ�λ����ʼ��� */

	/* ������Ҫ����ļ�ӿ��� */
	indirect_blks = (chain + depth) - partial - 1;

	/* count Ϊ��Ҫ�����ֱ�ӿ���: 1 */
	count = hdd_blks_to_allocate(partial, indirect_blks,
				maxblocks, blocks_to_boundary);
	/* ��ʵ��·�����з��� */
	err = hdd_alloc_branch(inode, indirect_blks, &count, goal,
				offsets + (partial - chain), partial);
	mutex_unlock(&hi->truncate_mutex);

	if (err) 
		goto cleanup;

	set_buffer_new(bh_result);
got_it:
	/* ���� inode �еĵ�ַ�����Կ��, ���·��ʼ���, ����������λ��:SSD/HDD */
	location = access_info_add(inode, chain+depth-1, offsets[depth-1]);

	/* ����ʵ��λ��, ��ɼ�¼ [�豸+ʵ�ʿ��] �� bh_result �� */
	if(location == BLOCK_ON_SSD) {
		set_buffer_mapped(bh_result);
		bh_result->b_bdev = sbi->ssd_bdev;
		bh_result->b_blocknr = le32_to_cpu(chain[depth-1].key);
		bh_result->b_size = inode->i_sb->s_blocksize;
	} else {/* δǨ�� */
		map_bh(bh_result, inode->i_sb, le32_to_cpu(chain[depth-1].key));
	}

	/* ��¼������Ƿ�Ϊһ�������е����һ����: �� 11, 797 �� */
	if (location == BLOCK_ON_HDD && count > blocks_to_boundary)
		set_buffer_boundary(bh_result);
	err = count; /* ����ֵΪ��ȡ��ֱ�ӿ��� */

	partial = chain + depth - 1;	/* the whole chain */
cleanup:
	while (partial > chain) { /* �ͷ�·���ϵļ�����ݿ� */
		brelse(partial->bh);
		partial--;
	}
	return err;
}

/* ��ȡ��Կ��Ϊ iblock �Ŀ��ʵ�ʿ��, ��¼�� bh �� */
int hdd_get_block(struct inode *inode, sector_t iblock, 
	struct buffer_head *bh_result, int create)
{
	/* ��ȡ�Ŀ��� : 1 */
	unsigned max_blocks = bh_result->b_size >> inode->i_blkbits;
	/* ��ʼ��ȡ, ����ֵΪ��ȡ�Ŀ��� */
	int ret/*1*/ = hdd_get_blocks(inode, iblock, max_blocks,/*1*/
		bh_result, create);

	if (ret > 0) {
		bh_result->b_size = (ret << inode->i_blkbits);
		ret = 0;
	}

	return ret;/* 0 Ϊ�ɹ� */
}

/* �ͷ�һЩֱ�����ݿ�, ��ΧΪ [p,q) */
static inline void hdd_free_data(struct inode *inode, __le32 *p, __le32 *q)
{
	unsigned long block_to_free = 0, count = 0;
	unsigned long nr = 0; /* ��� */
	unsigned int freed = 0;
	struct hdd_sb_info sbi = HDD_SB(inode->i_sb);

	for ( ; p < q ; p++) {
		nr = le32_to_cpu(*p);
		if (nr) {/*����Ų�Ϊ0, ����HDD��, �����Ų�����,����SSD�ϱ��ͷ� */
			*p = 0;
			/* accumulate blocks to free if they're contiguous */
			if (count == 0)
				goto free_this;
			else if (block_to_free == nr - count)
				count++;
			else {
				mark_inode_dirty(inode);
				/* �ڿ�λͼ���ͷŴ� block_to_free ��ʼ�� count ���� */
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
		/* �ڿ�λͼ���ͷŴ� block_to_free ��ʼ�� count ���� */
		hdd_free_blocks (inode, block_to_free, count);
		freed += count;
	}

	down_read(&sbi->sbi_rwsem);
	percpu_counter_sub(&sbi->usr_blocks, freed);
	up_read(&sbi->sbi_rwsem);	
}

/* ���[p, q) ��Χ���Ƿ�ȫΪ 0 */
static inline int all_zeroes(__le32 *p, __le32 *q)
{
	while (p < q)
		if (*p++)
			return 0;
	return 1;
}

/* ���� partial truancate �ļ�ӿ� */
static Indirect *hdd_find_shared(struct inode *inode, int depth,
	int offsets[4], Indirect chain[4], __le32 *top)
{
/* ����: ָ�������ͱ����鹲��һ����֧
 * @chain:  place to store the pointers to partial indirect blocks
 * @top:    place to the (detached) top of branch
 *
 * �� truncate() ʱ, ������Ҫ�������ӿ��β��ʱ,ʹ��ӿ��Լ���Ч. 
 * ���һЩ���� i_size �����ݿ������˼�ӿ�,���ӿ��Ǳ����ֽضϵ�.
 * ����Ҫ�ͷ����·���� top �� ���Ҷ˵Ŀ�. �� truncate()����֮ǰ, û�����ݷ����Խ��
 * truncation point. ����֧�� top ��Ҫ�ر�ע��, ��Ϊ�Ͳ�������ҳ���ܻ�ʹ����.
 */
	Indirect *partial = NULL; /* ָ�������·�� */
	Indirect *p = NULL;/* ָ����Զ�Ĺ���·�� */
	int k = 0;
	int err = 0;
	int addr_offset = 0;

	*top = 0;
	/* �Ӻ���ǰ,�ҵ�����һ���е��׸������ */
	for (k = depth; k > 1 && !offsets[k-1]; k--)
		;
	/* ȡ�ù���·�� */
	partial = hdd_get_branch(inode, k, offsets, chain, &err);

	/* 1.������п鶼�ѷ���, ����Զ����·��Ϊ����� */
	if (!partial)
		partial = chain + k-1;

	write_lock(&HDD_I(inode)->i_meta_lock);

	/* 2.���partial ָ��Ŀ鲢δ����, �򷵻� partial, �� *top Ϊ0 */
	if (!partial->key && *partial->p) {
		write_unlock(&HDD_I(inode)->i_meta_lock);
		goto no_top; /* ֱ��ȥ�ͷŹ�����ӿ� */
	}

	/* ����ѹ���·��ָ�������·�� */
	p = partial;
	/* ��ͬһ���� p->p ֮ǰ�Ŀ�Ŷ�Ϊ 0, ����δ����, ��ǰ�� p, ���̹���·�� */
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
		/* ����Զ������ַ����������Ҳ��ǵ�һ��, ��û�� top, 
		������ p ָ�����һ����Ч��ַ, ʹ���ͷŹ�����ַ��ʱ���� + 1 ����ʼ�ͷ� */
		p->p--; 
	} else {/* ����Զ������ַ�鲻�������, ���ǵ�һ�� */
		*top = *p->p;/* ���¼�����ͷſ��, ��������֮ */
		*p->p = 0;/* Ȼ���ͷŴ˵�ַ, ʹ��֮���ͷŹ�����ַ��ʱ, ����һ���鿪ʼ�ͷ� */
	}

	write_unlock(&HDD_I(inode)->i_meta_lock);

	while(partial > p) {	/* �ͷ� p ֮��Ļ���� */
		brelse(partial->bh);
		partial--;
	}
no_top:
	return partial /* p */;
}

/* �ͷ�һ���֧ [p,q) */
static void hdd_free_branches(struct inode *inode,
	__le32 *p, __le32 *q, int depth, char *data)
{
/* @p: �������ŵ�����
 * @q: ����ĩ�˺��λ��
 * @depth: Ԫ�ر�ʾ�ķ�֧����� - 1, 2, 3
 * data: ��ַ��ĵ�ַ, ���� bh->b_data
 */
	struct buffer_head * bh = NULL;
	unsigned long nr = 0;

	if (depth--) {/* �ͷ��м�����ַ�� */
		int addr_offset = 0;
		if (0 == depth) 
			addr_offset = HDD_ACCESS_END;

		for ( ; p < q ; p++) {
			nr = le32_to_cpu(*p); /* �ͷŵĿ�� */
			if (!nr) /* ���Ϊ 0, û������, �ͷ���һ��� */
				continue; 
			*p = 0; /* �������· */

			bh = sb_bread(inode->i_sb, nr); /* ȡ�õ�ַ������ */
			if (!bh) {
				hdd_msg(inode->i_sb, KERN_ERR,"hdd_free_branches",
					"Read failure, inode=%ld, block=%ld",
					inode->i_ino, nr);
				continue;
			}

			hdd_free_branches(inode, /* �ͷſ������е�ַ */
					  (__le32*)(bh->b_data + addr_offset),
					  (__le32*)bh->b_data + 1024,
					  depth,
					  bh->b_data);
			bforget(bh); /* ������д���ͷŵ��м����Ŀ� */
			hdd_free_blocks(inode, nr, 1); /* �ͷſ�ؿ�λͼ�� */
			mark_inode_dirty(inode);
		}
	} else {/* �ͷ����һ����ַ�� */
		/* �ͷŷ�����Ϣ�� SSD �ϵĿ�: 
		inode, ��ַ����ʼ��ַ, �׸��ͷſ��ַƫ��, �ͷŵĿ��� */
		access_info_sub(inode, data, p - (*__le32) data, q - p);

		hdd_free_data(inode, p, q);
	}
}

/* �ͷŴ������ݿ�, �� inode ��ָ���Ĵ�С */
void hdd_trancate(struct inode *inode)
{
	__le32 *i_data = HDD_I(inode)->i_data;
	struct hdd_inode_info *hi = HDD_I(inode);
	long iblock;		/* ��Կ�� */
	unsigned blocksize;	/* �鳤 */
	int offsets[4];		/* ��Կ��·�� */
	int n;			/* ·����� */
	Indirect chain[4];	/* ʵ�ʿ��·�� */
	Indirect *partial;	/* ʵ��·���еĴ������ */
	__le32 nr = 0;		/* ��� */
	__le32 *p = NULL;	/* ����ڵ�ַ���еĵ�ַ */
	__le32 **addr = &p;

	/* ֻ��������,Ŀ¼,�ǿ��ٷ������� */|
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) |
	    S_ISLNK(inode->i_mode)))
		return;
	if (S_ISLNK(inode->i_mode) && inode->i_blocks == 0)//
		return;
	/* ��ֻ�����ļ�, �ǲ��ɸ����ļ� */
	if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
		return;

	/* �鳤��Ҫ�ضϵ���ʼ��Կ�� */
	blocksize = inode->i_sb->s_blocksize;
	iblock = (inode->i_size + blocksize-1) >> HDD_BLOCK_LOG_SIZE;

	/* �� inode �е������޸�Ϊ i_size ��С:·�������������,+ ΢�� */
	block_truncate_page(inode->i_mapping,
			inode->i_size, hdd_get_block);

	/* ȡ�� iblock �����·��, ������� */
	n = hdd_block_to_path(inode, iblock, offsets, NULL);
	if (n == 0)
		return;

	mutex_lock(&hi->truncate_mutex);

	/* ���ض���ʼ����ֱ�ӿ���, �����ͷŶ���ֱ�ӿ� */
	if (n == 1) {
		/* �ͷ�λ�úͷ�����Ϣ */
		access_info_sub(inode, i_data, offsets[0], 
			HDD_NDIR_BLOCKS - offsets[0]);
		/* �ͷ����ݿ� */
		hdd_free_data(inode, i_data+offsets[0],
				i_data + HDD_NDIR_BLOCKS);
		goto do_indirects; /* ��ȥ�ͷ����м�ӿ� */
	}

	/* ȡ�ù���·�� chain, 
	   ������������ͬʱ�������ͷſ�ͱ�����ĵ�ַ�� (��ʱ, n > 1) */
	partial = hdd_find_shared(inode, n, offsets, chain, &nr);

	/* nr ��¼��Ҫ�ͷŵĿ�,Ҫ�ͷŴ˿������е�ַ
	Kill the top of shared branch (already detached) */
	if (nr) {
		if (partial == chain) {
			/* top Ϊ��ַ���еļ�ӵ�ַ (i_data ��ֵ�ѱ��� 0) */
			mark_inode_dirty(inode);
			hdd_free_branches(inode, &nr, &nr+1, n-1, NULL);
		} else {/* �ͷ��м����е�λ�� */
			mark_buffer_dirty_inode(partial->bh, inode);
			/* �� nr != 0, �� partial != chain, �� partial �� < chain+n-1,
			   �� (chain+n-1) - partial > 0 */
			hdd_free_branches(inode, &nr, &nr+1, 
				(chain+n-1) - partial, NULL);
		}
	}

	/* �ͷŷ�֧�еĹ�����ӿ��еĵ�ַ */
	while (partial > chain) {/* partial һ��ָ��ֱ���� */
		hdd_free_branches(inode,
				  partial->p + 1,/* +1 */
				  (__le32*)partial->bh->b_data + 1024,
				  (chain+n-1) - partial, partial->bh->b_data);

		mark_buffer_dirty_inode(partial->bh, inode);
		brelse (partial->bh);
		partial--;
	}

do_indirects:
	/* �ͷ�ʣ��ķǹ�����֧��ַ�� */
	switch (offsets[0]) {
		default: /* �� truncate ֱ�ӿ� */
			nr = i_data[HDD_IND_BLOCK];/* ���ͷ�һ���� */
			if (nr) {
				i_data[HDD_IND_BLOCK] = 0;
				mark_inode_dirty(inode);
				hdd_free_branches(inode, &nr, &nr+1, 1, NULL);
			}
		case HDD_IND_BLOCK: /* һ�μ�ַ */
			nr = i_data[HDD_DIND_BLOCK];/* Ȼ���ͷŶ����� */
			if (nr) {
				i_data[HDD_DIND_BLOCK] = 0;
				mark_inode_dirty(inode);
				hdd_free_branches(inode, &nr, &nr+1, 2, NULL);
			}
		case HDD_DIND_BLOCK:/* ���μ�ַ */
			nr = i_data[HDD_TIND_BLOCK];/* �ͷ������� */
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

/* ɾ�������ϵ� inode */
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
		hdd_truncate (inode); /* �ͷ� inode �Ĵ������ݿ� */

	hdd_free_inode (inode); /* �ͷ� inode ���� */
}

/* ͬ�� inode */
int hdd_sync_inode(struct inode *inode)
{
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = 0,	/* sys_fsync did this */
	};
	return sync_inode(inode, &wbc);
}

/* ��ʼ��������Ϣ */
void access_info_init(struct inode *inode, 
	struct buffer_head *bh, unsigned int offset)
{
/*
  �����߳��� inode->truncate_lock,
  �ڵ��ú��� bh �� inode Ϊ��.
 
  bh ��Ϊ NULL, ��ʾ offset Ϊֱ�ӿ�ƫ��, ����Ϊ��ӿ�ƫ��.
 */

}

/* ���ӷ��ʼ���, �����ʼ���������ƽ��ֵ, ��Ǩ�Ƶ� SSD, ����λ�� */
int access_info_add(inode, Indirect *branch, unsigned int offset)
{
/*
  branch.bh ��Ϊ NULL, ��ʾ offset Ϊֱ�ӿ�ƫ��, ����Ϊ��ӿ�ƫ��.
  
  
  ����Ŀ¼�ļ�, ��һ������Ǩ��;

  ��ԭ����HDD��, �����SSD�ϵķ�����Ϣ, ���ʼ����, ���ж��Ƿ���Ҫ��Ǩ��
	  branch->key ��¼�˿��, ���ڴ���Ǩ��, ����Ҫ������ֵ.����� bh Ϊ��.

  ��ԭ����SSD��, �����SSD�ϵķ�����Ϣ,

  ���� 1 ��ʾ��SSD��, 0 ��ʾ�� HDD ��
 */
struct buffer_head *bh;
}

/* �ͷ����ݿ�, ��������ʼ���, ������SSD��, ���ͷſ�, ����ǿ��Ϊ 0 */
void access_info_sub(struct inode *inode, __le32 *data, int offset, int count)
{
	/* data Ϊ��ַ����ʼ��ַ, ������ i_data, ���ʾֱ�ӿ�, ����Ϊ��ַ�������һ��.
	 * offset ��ʾҪ�ͷŵ���ʼ���� data �е�ƫ��.
	 * count ��ʾҪ�ͷŵĿ���.
	 */


}

/* �����ļ��������� */
int hdd_setattr(struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	int error;

	error = inode_change_ok(inode, iattr); /* ���Ȩ�� */
	if (error)
		return error;

	error = inode_setattr(inode, iattr);

	return error;
}

/* ��ȡһҳ */
static int hdd_readpage(struct file *file, struct page *page)
{
	return mpage_readpage(page, hdd_get_block);
}

/* ��ȡ��ҳ */
static int hdd_readpages(struct file *file, struct address_space *mapping,
	struct list_head *pages, unsigned nr_pages)
{
	return mpage_readpages(mapping, pages, nr_pages, hdd_get_block);
}

/* дһҳ */
static int hdd_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, hdd_get_block, wbc);
}

/* д��ҳ */
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

/* �����ļ�ϵͳ�Ϳ�� */
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

	.set_page_dirty		= f2fs_set_data_page_dirty,
	.invalidatepage		= f2fs_invalidate_data_page,
	.releasepage		= f2fs_release_data_page,

	.is_partially_uptodate	= block_is_partially_uptodate,
	.error_remove_page	= generic_error_remove_page,
};
