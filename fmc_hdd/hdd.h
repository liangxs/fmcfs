/*
 * fmcfs/fmc_hdd/hdd.h
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * @ Beijing University of Posts and Telecommunications,
 * @ CPU & SoC Center of Tsinghua University.
 *
 * This program can be redistributed under the terms of the GNU Public License.
 */

#ifndef __LINUX_FS_FMC_HDD_H__
#define __LINUX_FS_FMC_HDD_H__

#include <linux/rwsem.h>
#include <linux/rbtree.h>
#include <linux/seqlock.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/blockgroup_lock.h>
#include <linux/percpu_counter.h>

#include "../fmc_fs.h"
#include "../fmc_cld/cld.h"
#include "../fmc_ssd/ssd.h"

#define HDD_FT_UNKNOWN		0		/* �ļ����� */
#define HDD_FT_REG_FILE		1
#define HDD_FT_DIR		2
#define HDD_FT_CHRDEV		3
#define HDD_FT_BLKDEV		4
#define HDD_FT_FIFO		5
#define HDD_FT_SOCK		6
#define HDD_FT_SYMLINK		7
#define HDD_FT_MAX		8

#define HDD_SECTOR_SIZE		512		/* ������С */
#define HDD_SECTORS_PER_BLK	8		/* ÿ�� 8 �� */

#define HDD_INODE_SIZE		128		/* ������ inode ��С */
#define HDD_ROOT_INO		2		/* ��Ŀ¼ ino */
#define HDD_FIRST_INO		11		/* �׸��Ǳ��� ino */

#define	HDD_NDIR_BLOCKS		12		/* ֱ�ӵ�ַ���� */
#define	HDD_IND_BLOCK		HDD_NDIR_BLOCKS	/* ��ӿ��-12 */
#define	HDD_DIND_BLOCK		(HDD_IND_BLOCK+1)/* ���μ�ַ���-13 */
#define	HDD_TIND_BLOCK		(HDD_DIND_BLOCK+1)/* ���μ�ַ���14 */
#define	HDD_N_BLOCKS		(HDD_TIND_BLOCK+1)/* ��ַ���� */

#define DEF_HDD_MAXRATIO	90		/* Ĭ�� hdd ���ʹ���� */
#define DEF_HDD_MAXAGE		2147483647	/* ��󲻷�����Ǩ��ʱ�� */

#define	HDD_MOUNT_FS		0x00000000	/* ��װ��, ��δ�ɾ�ж�� */
#define	HDD_VALID_FS		0x00000001	/* �ɾ���ж�� */
#define	HDD_ERROR_FS		0x00000002	/* ���� */

/* inode ��־: i_flags */
#define	HDD_SECRM_FL		0x00000001	/* Secure deletion */
#define	HDD_UNRM_FL		0x00000002	/* Undelete */
#define	HDD_COMPR_FL		0x00000004	/* Compress file */
#define HDD_SYNC_FL		0x00000008	/* Synchronous updates */
#define HDD_IMMUTABLE_FL	0x00000010	/* ���ɸ����ļ� */
#define HDD_APPEND_FL		0x00000020	/* writes to file may only append */
#define HDD_NODUMP_FL		0x00000040	/* do not dump file */
#define HDD_NOATIME_FL		0x00000080	/* do not update atime */

#define HDD_IF_ONHDD		0x00000100	/* ȫ�� HDD �� */
#define HDD_IF_ONBOTH		0x00000200	/* �� HDD �� SSD �� */
#define	HDD_IF_ONSSD		0x00000400	/* ȫ�� SSD �� */
#define HDD_IF_ONCLD		0x00000800	/* �� CLD �� */

#define HDD_INDEX_FL		0x00001000	/* hash-indexed directory */
#define HDD_IMAGIC_FL		0x00002000	/* AFS directory */
#define HDD_JOURNAL_DATA_FL	0x00004000	/* file data should be journaled */
#define HDD_NOTAIL_FL		0x00008000
#define HDD_DIRSYNC_FL		0x00010000	/* dirsync behaviour (directories only) */
#define HDD_TOPDIR_FL		0x00020000	/* Top of directory hierarchies*/
#define HDD_RESERVED_FL		0x80000000
#define HDD_FL_USER_VISIBLE	0x0003DFFF	/* User visible flags */
#define HDD_FL_USER_MODIFIABLE	0x00038FFF	/* User modifiable flags */

#define HDD_REG_FLMASK (~(HDD_DIRSYNC_FL | HDD_TOPDIR_FL))
#define HDD_OTHER_FLMASK (HDD_NODUMP_FL | HDD_NOATIME_FL)

/* �� inode Ӧ�ôӸ��׼̳еı�־ */
#define HDD_FL_INHERITED (\
	HDD_SECRM_FL | HDD_UNRM_FL | HDD_COMPR_FL |\
	HDD_SYNC_FL  | HDD_IMMUTABLE_FL | HDD_APPEND_FL |\
	HDD_NODUMP_FL | HDD_NOATIME_FL  | HDD_JOURNAL_DATA_FL |\
	HDD_NOTAIL_FL | HDD_DIRSYNC_FL)

/* inode ����״̬ */
#define HDD_STATE_JDATA			0x00000001 /* ��־���� - journaled data exists */
#define HDD_STATE_NEW			0x00000002 /* �´��� - inode is newly created */
#define HDD_STATE_XATTR			0x00000004 /* has in-inode xattrs */
#define HDD_STATE_FLUSH_ON_CLOSE	0x00000008

#define HDD_LINK_MAX		32000		/* ���Ӳ������/��Ŀ¼�ļ��� */

#define HDD_BLOCK_SIZE		4096		/* �鳤 */
#define HDD_BLOCK_LOG_SIZE	12		/* �鳤 log */

#define	HDD_ADDR_PER_BLOCK	798		/* ���е�ַ�� */
#define	HDD_ADDR_SIZE		3192		/* ��ַ���� */
#define HDD_ADDR_BMAP_END	104		/* ��ַλͼ�յ�, 100B ��Ч */
#define HDD_ACCESS_END		904		/* ͳ����Ϣ�յ�, 798B ��Ч */
#define HDD_ADDR_END		4096		/* ��ַ��Ϣ�յ� */

struct hdd_super_block {
/*00*/	__le32		s_magic;		
	__le16		s_major_ver;		/* �ļ�ϵͳ�汾 */
	__le16		s_mimor_ver;
	__le32		s_groups_count;		/* group ���� */
	__le32		s_blocks_count;		/* block ���� */
/*10*/	__le32		s_inodes_count;		/* inode ���� */
	__le32		s_gdt_blocks;		/* ������������ռ���� */
	__le32		s_itable_blocks;	/* inode ����ռ���� */
	__le32		s_user_blocks;		/* �û����õĴ����ݵĿ���(���Ǽ�ӿ�) */
/*20*/	__le32		s_free_blocks_count;	/* ���� block �� */
	__le32		s_free_inodes_count;	/* ���� inode �� */
	__le32		s_block_size;		/* block �ֽ��� */
	__le32		s_inode_size;		/* inode �ֽ��� */
/*30*/	__le32		s_blocks_per_group;	/* �����е� block ���� (ȫ��)*/
	__le32		s_last_group_blocks;	/* ���һ��������ܿ��� */
	__le32		s_inodes_per_group;	/* �����е� inode �� */
	__le32		s_first_data_block;	/* �׸����� block ��: 1 (��0������) */
/*40*/	__le32		s_first_ino;		/* �׸����� inode ��: 11 */	
	__le32		s_log_block_size;	/* �鳤: 4096 * (2^#) */
	__le32		s_block_group_nr;	/* �˳��������ڵĿ��� */
	__le32		s_mkfs_time;		/* �ļ�ϵͳ����ʱ�� */
/*50*/	__le32		s_mtime;		/* �ϴ�װ��ʱ�� */
	__le32		s_wtime;		/* �ϴ�д��ʱ�� */
	__le32		s_state;		/* 0-δ�ɾ�ж�ػ�װ�� */
	__le16		s_prealloc_blocks;	/* ��ͼԤ������� (�����ļ�)*/
	__le16		s_prealloc_dir_blks;	/* ��ͼԤ������� (Ŀ¼�ļ�) */

/*60*/	__u8		s_uuid[16];		/* �� UUID */
/*70*/	__u8		s_ssd_uuid[16];		/* Ŀ�� ssd �� UUID */
/*80*/	char		s_ssd_name[16];		/* Ŀ�� ssd ���豸��,  */
/*90*/ 	char		s_cld_name[CLD_NAME_LEN];/* �ƴ洢�ṩ������ */
	__le32		s_ssd_blocks_count;	/* Ǩ�Ƶ� SSD �Ŀ��� */
	__le32		s_hdd_idx;		/* �� ssd �� uuid ������±� */
	__le64		s_cld_blocks_count;	/* Ǩ�Ƶ� Cloud �Ŀ��� */
/*A0*/	__le32		s_cld_files_count;	/* Ǩ�Ƶ� Cloud ���ļ��� */

	__le32		s_upper_ratio;		/* ����Ǩ�ƵĿռ�ʹ���� */
	__le32		s_max_unaccess;		/* ����Ǩ�Ƶ��ļ��������� - �� */
	__le64		s_total_access;		/* ���ݿ���ܷ��ʴ��� */
	__u32		s_pad1[78];		/* ��䵽 512 �ֽ� */
	__le32		s_blks_per_level[FMC_MAX_LEVELS];/* Լ1K-ÿ�����ʼ���Ŀ��� */
	__le32		s_pad2[6];
};

struct hdd_sb_info {
	struct rw_semaphore	sbi_rwsem;	/* �����䲿��ʱ, ����Ӵ���;
						   ����ɱ��ʱ,���ȼӴ˶���, �ټӸ�����; 
						   д������ʱ,��д�� */

	struct super_block	*sb;
	struct buffer_head	*hdd_bh;
	struct hdd_super_block	*hdd_sb;
	unsigned int		sb_block;	/* ���������ڿ�� */
	int			s_dirty;	/* ˽����Ϣ�Ƿ�Ϊ�� */

	unsigned int		mount_opt;	/* �Զ���Ĺ���ѡ�� */
	unsigned int		mount_state;	/* ����״̬ */

	struct buffer_head	**group_desc;	/* ����������Ļ���ͷ */
	struct blockgroup_lock	*blkgrp_lock;	/* ���������� */
	/* ���䲿�� */
	unsigned int		groups_count;	/* ������ */
	unsigned int		blocks_count;	/* ������ */
	unsigned int		inodes_count;	/* inode ���� */
	unsigned int		gdt_blocks;	/* ����������ռ���� */
	unsigned int		itable_blocks;	/* ������ inode �Ŀ��� */
	unsigned int		inode_size;	/* inode ��С */	
	unsigned int		block_size;	/* ���С */
	unsigned int		log_blocksize;	/* ���С log */
	unsigned int		blks_per_group;	/* �����е� block ���� */
	unsigned int		last_group_blks;/* ��������ܿ��� */
	unsigned int		inodes_per_group;/* �����е� inode �� */
	unsigned int		inodes_per_block;/* ���е� inode �� */
	unsigned int		desc_per_block;	/* ÿ�����е������������� */
	unsigned int		desc_per_blk_bits;/* ÿ���е��������� */
	unsigned int		addr_per_blk;	/* ÿ���еĵ�ַ���� */
	unsigned int		root_ino_num;	/* �� inode �� */
	unsigned int		first_ino;	/* �׸��Ǳ��� inode �� */
	unsigned int		grp_data_offset;/* �������û����ݿ��ƫ�ƿ��� */

	/* �仯���� */
	struct percpu_counter	usr_blocks;	/* �û����õĴ����ݵĿ��� */
	struct percpu_counter	free_blks_count;/* ���п���� */
	struct percpu_counter	free_inodes_count;/* ���� inode ���� */
	struct percpu_counter	total_access;	/* ���ݿ���ܷ��ʴ��� */
	struct percpu_counter	*blks_per_lvl;	/* ÿ���ʼ����еĿ��� */
	struct percpu_counter	ssd_blks_count;	/* Ǩ�Ƶ� SSD �Ŀ��� */

	struct mutex		ssd_mutex;	/* ���Ʒ��� ssd_info */	
	struct ssd_sb_info	*ssd_info;	/* ����Ӧ�� ssd ��Ϣ */
	struct block_device	*ssd_bdev;	/* ssd �Ŀ��豸��Ϣ */
	char			cld_name[16];	/* �ƴ洢���� */
	int			hdd_idx;	/* �� ssd �� uuid ������±� */

	spinlock_t		cld_lock;	/* ���Ʒ�������Ϣ */
	u64			cld_blks_count;	/* Ǩ�Ƶ� Cloud �Ŀ��� */
	unsigned int		cld_files_count;/* Ǩ�Ƶ� Cloud ���ļ��� */
	
	unsigned int		upper_ratio;	/* ����Ǩ�ƿ�Ŀռ�ʹ���� */
	unsigned int		max_unaccess;	/* ����Ǩ���ļ���δ�������� - �� */
};

struct hdd_inode {
/*00*/	__le16		i_mode;			/* �ļ����ͺͷ���Ȩ�� */
	__le16		i_links_count;		/* Ӳ������ */
	__le32		i_flags;		/* �ļ���־��Ǩ�Ʊ�־*/
	__le32		i_uid;			/* Owner Uid */
	__le32		i_gid;			/* Group Id */
/*10*/	__le64		i_size;			/* �ļ����� */
	__le32		i_blocks;		/* ���� */
	__le32		i_access_count;		/* ����ͳ����Ϣ */
/*20*/	__le32		i_atime;		/* ����ʱ�� */
	__le32		i_ctime;		/* �ϴ��޸� inode ʱ�� */
	__le32		i_mtime;		/* �޸�ʱ�� */
	__le32		i_dtime;		/* ɾ��ʱ�� */
/*30*/	union {
	struct{
	__le64		i_cld_id;		/* �����е� ID */
	char		cld_info[72];		/* �����е���Ϣ */
	}s_cloud;
	struct{
	__le32		i_ssd_blocks;		/* �� ssd �еĿ��� */
	__le16		i_pad;
	__le16		i_direct_bits;		/* ǰ12�����λ�ñ�־ */
	__u8		i_direct_blks[HDD_NDIR_BLOCKS];/* ǰ12����ķ��ʼ��� */
	__le32		i_block[HDD_N_BLOCKS];	/* ��ַ����[15] */
	}s_hdd;
	}u;
};

struct hdd_inode_info {
	struct inode	vfs_inode;		/* vfs inode */
	__le32		i_data[15];		/* �ļ����ݵ�ַ */
	unsigned int	i_flags;		/* �ļ���־��Ǩ�Ʊ�־ */
	unsigned int	i_state;		/* inode ״̬: STATE_NEW.. */
	unsigned int	i_dtime;		/* ɾ��ʱ�� */
	unsigned int	i_block_group;		/* inode ���ڿ���� */
	unsigned int	i_dir_start_lookup;	/* ��ʼ���ҵ�Ŀ¼ */

	unsigned int	i_access_count;		/* ���ʼ��� */
	unsigned int	i_ssd_blocks;		/* �� SSD �еĿ��� */
	__u16		i_direct_bits;		/* ǰ12�����λ�ñ�־ */
	__u16		i_pad;
	__u8		i_direct_blks[HDD_NDIR_BLOCKS];	/* ǰ12����ķ��ʼ��� */
					
	struct mutex	truncate_mutex;		/* �������л� hdd_truncate, hdd_getblock */
	rwlock_t	i_meta_lock;
	struct list_head i_orphan;		/* unlinked but open inodes */
};

struct hdd_group_desc {
	__le32		bg_block_bitmap;	/* ��λͼ�Ŀ�� */
	__le32		bg_inode_bitmap;	/* inode λͼ�Ŀ�� */
	__le32		bg_inode_table;		/* inode �����ʼ��� */
	__le32		bg_free_blocks_count;	/* ���п��� */
	__le32		bg_free_inodes_count;	/* ���� inode �� */
	__le32		bg_used_dirs_count;	/* Ŀ¼���� */
	__le32		bg_reserved[2];		/* ���� */
};

struct hdd_dir_entry {
	__le32		ino;			/* inode �� */
	__le16		rec_len;		/* dentry ���� */
	__u8		name_len;		/* �ļ���ʵ�ʳ��� */
	__u8		file_type;		/* �ļ�����*/
	char		name[HDD_NAME_LEN];	/* 255,�ļ��� */
};

typedef struct hdd_dir_entry hdd_dirent;

/* ȡ�ÿ����� */
static inline spinlock_t * sb_bgl_lock(struct hdd_sb_info *sbi,
	unsigned int block_group)
{
	return bgl_lock_ptr(sbi->blkgrp_lock, block_group);
}

static inline struct hdd_sb_info *HDD_SB(struct super_block *sb)
{
	return (struct hdd_sb_info *) sb->s_fs_info;
}

static inline struct hdd_inode_info *HDD_I(struct inode *inode)
{
	return container_of(inode, struct hdd_inode_info, vfs_inode);
}

/* inode ���ڴ�ʹ����ϵ�λ�� */
struct hdd_iloc {
	struct buffer_head *bh;
	unsigned long offset;
	unsigned long block_group;
};

/* ȡ�� hdd inode ����*/
static inline struct hdd_inode *hdd_raw_inode(struct hdd_iloc *iloc)
{
	return (struct hdd_inode *) (iloc->bh->b_data + iloc->offset);
}

#define HDD_BLOCKS_PER_GROUP(s)	(HDD_SB(s)->blks_per_group)
#define HDD_DESC_PER_BLOCK(s)	(HDD_SB(s)->desc_per_block)
#define HDD_INODES_PER_GROUP(s)	(HDD_SB(s)->inodes_per_group)
#define HDD_DESC_PER_BLOCK_BITS(s) (HDD_SB(s)->desc_per_blk_bits)

/* ���������ʼ��� */
static inline unsigned int
hdd_group_first_block_no(struct super_block *sb, unsigned long group_no)
{
	return group_no * (unsigned int)HDD_BLOCKS_PER_GROUP(sb) +
		le32_to_cpu(HDD_SB(sb)->hdd_sb->s_first_data_block);
}

/* ��� ino �Ƿ�Ϸ� */
static inline int hdd_valid_inum(struct super_block *sb, unsigned long ino)
{
	return  ino == HDD_ROOT_INO ||
	       (ino >= HDD_FIRST_INO &&
		ino <= le32_to_cpu(HDD_SB(sb)->hdd_sb->s_inodes_count));
}

/* ����ѡ�� */
#define HDD_MOUNT_CHECK			0x00001	/* װ��ʱ��� */
#define HDD_MOUNT_DEBUG			0x00008	/* һЩ������Ϣ */

/* ���, ����, ���Թ���ѡ�� */
#define clear_opt(o, opt)		o &= ~HDD_MOUNT_##opt
#define set_opt(o, opt)			o |= HDD_MOUNT_##opt
#define test_opt(sb, opt)		(HDD_SB(sb)->s_mount_opt & \
					HDD_MOUNT_##opt)

/* ��ϣ��Ŀ¼���� */
#define is_dx(dir) ((HDD_I(dir)->i_flags & HDD_INDEX_FL))
#define HDD_DIR_LINK_MAX(dir) (!is_dx(dir) && (dir)->i_nlink >= HDD_LINK_MAX)
#define HDD_DIR_LINK_EMPTY(dir) ((dir)->i_nlink == 2 || (dir)->i_nlink == 1)

/* hash info structure used by the directory hash */
struct dx_hash_info{
	u32		hash;		/* ��ϣֵ */
	u32		minor_hash;	/*  */
	int		hash_version;	/* ���õĹ�ϣ�汾 */
	u32		*seed;		/* �Զ������� */
};

#define HDD_HTREE_EOF	0x7fffffff

/* Control parameters used by hdd_htree_next_block */
#define HASH_NB_ALWAYS		1

static inline __u32 hdd_mask_flags(umode_t mode, __u32 flags)
{
	if (S_ISDIR(mode))
		return flags;
	else if (S_ISREG(mode))
		return flags & HDD_REG_FLMASK;
	else
		return flags & HDD_OTHER_FLMASK;
}

extern void hdd_msg(struct super_block *sb, const char *level, 
	     const char *func, const char *fmt, ...);

/* �������� - balloc.c */
extern int hdd_bg_has_super(struct super_block *sb, int group);
extern unsigned long hdd_bg_num_gdb(struct super_block *sb, int group);
extern unsigned int hdd_new_block (struct inode *inode, unsigned int goal,
				 int *errp);
extern unsigned int hdd_new_blocks (struct inode *inode, unsigned int goal, 
				   unsigned long *count, int *errp);
extern void hdd_free_blocks (struct inode *inode, unsigned int block, 
			     unsigned long count);
extern void hdd_free_blocks_sb (struct super_block *sb, unsigned int block, 
			     unsigned long count, unsigned long *dquot_free);
extern unsigned int hdd_count_free_blocks (struct super_block *);
extern void hdd_check_blocks_bitmap (struct super_block *);
extern struct hdd_group_desc * hdd_get_group_desc(struct super_block * sb,
				unsigned int block_group, struct buffer_head ** bh);
extern int hdd_should_retry_alloc(struct super_block *sb, int *retries);
extern void hdd_init_block_alloc_info(struct inode *);

/* dir.c */
extern int hdd_check_dir_entry(const char *, struct inode *,
struct hdd_dir_entry *, struct buffer_head *, unsigned long);
extern int hdd_htree_store_dirent(struct file *dir_file, __u32 hash,
				  __u32 minor_hash, struct hdd_dir_entry *dirent);
extern void hdd_htree_free_dir_info(struct dir_private_info *p);

/* �ļ�ͬ������ - fsync.c */
extern int hdd_sync_file (struct file *, struct dentry *, int);

/* Ŀ¼��ϣ���� - hash.c */
extern int hddfs_dirhash(const char *name, int len, struct
			 dx_hash_info *hinfo);
/* inode ������� - ialloc.c */
extern struct inode * hdd_new_inode (struct inode *, int);
extern void hdd_free_inode (struct inode *);
extern struct inode * hdd_orphan_get (struct super_block *, unsigned long);
extern unsigned long hdd_count_dirs (struct super_block *);
extern void hdd_check_inodes_bitmap (struct super_block *);
extern unsigned long hdd_count_free (struct buffer_head *, unsigned);

/* �� inode �Ĳ��� - inode.c */
int hdd_forget(int is_metadata, struct inode *inode,
struct buffer_head *bh, unsigned int blocknr);
struct buffer_head * hdd_getblk (struct inode *, long, int, int *);
struct buffer_head * hdd_bread (struct inode *, int, int, int *);
int hdd_get_blocks_handle(struct inode *inode,
			  sector_t iblock, unsigned long maxblocks,
			  struct buffer_head *bh_result, int create);
extern struct inode *hdd_iget(struct super_block *, unsigned long);
extern int  hdd_write_inode (struct inode *, int);
extern void hdd_delete_inode (struct inode *);
extern int  hdd_sync_inode (struct inode *);
extern void hdd_discard_reservation (struct inode *);
extern void hdd_dirty_inode(struct inode *);
extern int  hdd_change_inode_journal_flag(struct inode *, int);
extern int  hdd_get_inode_loc(struct inode *, struct hdd_iloc *);
extern int  hdd_can_truncate(struct inode *inode);
extern void hdd_truncate (struct inode *);
extern void hdd_set_inode_flags(struct inode *);
extern void hdd_get_inode_flags(struct hdd_inode_info *);
extern void hdd_set_aops(struct inode *inode);
extern int  hdd_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		       u64 start, u64 len);
extern int hdd_setattr(struct dentry *dentry, struct iattr *iattr);

/* �������� - ioctl.c */
extern long hdd_ioctl(struct file *, unsigned int, unsigned long);

/* �¶��ļ� - namei.c */
extern int hdd_orphan_add(struct inode *);
extern int hdd_orphan_del(struct inode *);

extern int hdd_htree_fill_tree(struct file *dir_file, __u32 start_hash,
			       __u32 start_minor_hash, __u32 *next_hash);


/* hdd_namei.c */
extern struct dentry *hdd_get_parent(struct dentry *child);

/* inode.c */
extern int __hdd_write_begin(struct file *file, struct address_space *mapping,
		             loff_t pos, unsigned len, unsigned flags,
			     struct page **pagep, void **fsdata);

/* hdd_dir.c */
extern ino_t hdd_inode_by_name(struct inode *dir, struct qstr *child);
extern int hdd_add_link (struct dentry *dentry, struct inode *inode);
extern struct hdd_dir_entry *hdd_find_entry (struct inode * dir,
	struct qstr *child, struct page ** res_page);
extern int hdd_delete_entry (struct hdd_dir_entry * dir,
	struct page * page );
extern int hdd_make_empty(struct inode *inode, struct inode *parent);
extern int hdd_empty_dir (struct inode * inode);
extern struct hdd_dir_entry * hdd_dotdot (struct inode *dir, struct page **p);
extern void hdd_set_link(struct inode *dir, struct hdd_dir_entry *de,
	struct page *page, struct inode *inode, int update_times);

	/* �����뾯�� - super.c */

#define hdd_std_error(sb, errno)				\
	do {							\
		if ((errno))					\
		    __hdd_std_error((sb), __func__, (errno));	\
	} while (0)

extern const struct address_space_operations hdd_aops;
/* Ŀ¼�ļ� ���������� - dir.c */
extern const struct file_operations hdd_dir_operations;
/* �ļ����������� - file.c */
extern const struct file_operations hdd_file_operations;
extern const struct inode_operations hdd_file_inode_operations;
/* inode ���������� - namei.c */
extern const struct inode_operations hdd_dir_inode_operations;
extern const struct inode_operations hdd_special_inode_operations;
/* �������Ӳ��������� - symlink.c */
extern const struct inode_operations hdd_symlink_inode_operations;
extern const struct inode_operations hdd_fast_symlink_inode_operations;


#endif	/*__LINUX_FS_FMC_HDD_H__*/
