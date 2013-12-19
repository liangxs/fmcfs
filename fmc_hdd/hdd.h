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

#define HDD_FT_UNKNOWN		0		/* 文件类型 */
#define HDD_FT_REG_FILE		1
#define HDD_FT_DIR		2
#define HDD_FT_CHRDEV		3
#define HDD_FT_BLKDEV		4
#define HDD_FT_FIFO		5
#define HDD_FT_SOCK		6
#define HDD_FT_SYMLINK		7
#define HDD_FT_MAX		8

#define HDD_SECTOR_SIZE		512		/* 扇区大小 */
#define HDD_SECTORS_PER_BLK	8		/* 每块 8 扇 */

#define HDD_INODE_SIZE		128		/* 磁盘上 inode 大小 */
#define HDD_ROOT_INO		2		/* 根目录 ino */
#define HDD_FIRST_INO		11		/* 首个非保留 ino */

#define	HDD_NDIR_BLOCKS		12		/* 直接地址块数 */
#define	HDD_IND_BLOCK		HDD_NDIR_BLOCKS	/* 间接块号-12 */
#define	HDD_DIND_BLOCK		(HDD_IND_BLOCK+1)/* 二次间址块号-13 */
#define	HDD_TIND_BLOCK		(HDD_DIND_BLOCK+1)/* 三次间址块号14 */
#define	HDD_N_BLOCKS		(HDD_TIND_BLOCK+1)/* 地址块数 */

#define DEF_HDD_MAXRATIO	90		/* 默认 hdd 最大使用率 */
#define DEF_HDD_MAXAGE		2147483647	/* 最大不访问则迁移时间 */

#define	HDD_MOUNT_FS		0x00000000	/* 被装载, 或未干净卸载 */
#define	HDD_VALID_FS		0x00000001	/* 干净地卸载 */
#define	HDD_ERROR_FS		0x00000002	/* 出错 */

/* inode 标志: i_flags */
#define	HDD_SECRM_FL		0x00000001	/* Secure deletion */
#define	HDD_UNRM_FL		0x00000002	/* Undelete */
#define	HDD_COMPR_FL		0x00000004	/* Compress file */
#define HDD_SYNC_FL		0x00000008	/* Synchronous updates */
#define HDD_IMMUTABLE_FL	0x00000010	/* 不可更改文件 */
#define HDD_APPEND_FL		0x00000020	/* writes to file may only append */
#define HDD_NODUMP_FL		0x00000040	/* do not dump file */
#define HDD_NOATIME_FL		0x00000080	/* do not update atime */

#define HDD_IF_ONHDD		0x00000100	/* 全在 HDD 上 */
#define HDD_IF_ONBOTH		0x00000200	/* 在 HDD 和 SSD 上 */
#define	HDD_IF_ONSSD		0x00000400	/* 全在 SSD 上 */
#define HDD_IF_ONCLD		0x00000800	/* 在 CLD 上 */

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

/* 新 inode 应该从父亲继承的标志 */
#define HDD_FL_INHERITED (\
	HDD_SECRM_FL | HDD_UNRM_FL | HDD_COMPR_FL |\
	HDD_SYNC_FL  | HDD_IMMUTABLE_FL | HDD_APPEND_FL |\
	HDD_NODUMP_FL | HDD_NOATIME_FL  | HDD_JOURNAL_DATA_FL |\
	HDD_NOTAIL_FL | HDD_DIRSYNC_FL)

/* inode 对象状态 */
#define HDD_STATE_JDATA			0x00000001 /* 日志数据 - journaled data exists */
#define HDD_STATE_NEW			0x00000002 /* 新创建 - inode is newly created */
#define HDD_STATE_XATTR			0x00000004 /* has in-inode xattrs */
#define HDD_STATE_FLUSH_ON_CLOSE	0x00000008

#define HDD_LINK_MAX		32000		/* 最大硬链接数/子目录文件数 */

#define HDD_BLOCK_SIZE		4096		/* 块长 */
#define HDD_BLOCK_LOG_SIZE	12		/* 块长 log */

#define	HDD_ADDR_PER_BLOCK	798		/* 块中地址数 */
#define	HDD_ADDR_SIZE		3192		/* 地址长度 */
#define HDD_ADDR_BMAP_END	104		/* 地址位图终点, 100B 有效 */
#define HDD_ACCESS_END		904		/* 统计信息终点, 798B 有效 */
#define HDD_ADDR_END		4096		/* 地址信息终点 */

struct hdd_super_block {
/*00*/	__le32		s_magic;		
	__le16		s_major_ver;		/* 文件系统版本 */
	__le16		s_mimor_ver;
	__le32		s_groups_count;		/* group 总数 */
	__le32		s_blocks_count;		/* block 总数 */
/*10*/	__le32		s_inodes_count;		/* inode 总数 */
	__le32		s_gdt_blocks;		/* 组描述符表所占块数 */
	__le32		s_itable_blocks;	/* inode 表所占块数 */
	__le32		s_user_blocks;		/* 用户已用的纯数据的块数(不记间接块) */
/*20*/	__le32		s_free_blocks_count;	/* 空闲 block 数 */
	__le32		s_free_inodes_count;	/* 空闲 inode 数 */
	__le32		s_block_size;		/* block 字节数 */
	__le32		s_inode_size;		/* inode 字节数 */
/*30*/	__le32		s_blocks_per_group;	/* 块组中的 block 总数 (全部)*/
	__le32		s_last_group_blocks;	/* 最后一个块组的总块数 */
	__le32		s_inodes_per_group;	/* 块组中的 inode 数 */
	__le32		s_first_data_block;	/* 首个可用 block 号: 1 (块0启动块) */
/*40*/	__le32		s_first_ino;		/* 首个可用 inode 号: 11 */	
	__le32		s_log_block_size;	/* 块长: 4096 * (2^#) */
	__le32		s_block_group_nr;	/* 此超级块所在的块组 */
	__le32		s_mkfs_time;		/* 文件系统创建时间 */
/*50*/	__le32		s_mtime;		/* 上次装载时间 */
	__le32		s_wtime;		/* 上次写入时间 */
	__le32		s_state;		/* 0-未干净卸载或被装载 */
	__le16		s_prealloc_blocks;	/* 试图预分配块数 (常规文件)*/
	__le16		s_prealloc_dir_blks;	/* 试图预分配块数 (目录文件) */

/*60*/	__u8		s_uuid[16];		/* 卷 UUID */
/*70*/	__u8		s_ssd_uuid[16];		/* 目标 ssd 的 UUID */
/*80*/	char		s_ssd_name[16];		/* 目标 ssd 的设备名,  */
/*90*/ 	char		s_cld_name[CLD_NAME_LEN];/* 云存储提供商名字 */
	__le32		s_ssd_blocks_count;	/* 迁移到 SSD 的块数 */
	__le32		s_hdd_idx;		/* 在 ssd 中 uuid 数组的下标 */
	__le64		s_cld_blocks_count;	/* 迁移到 Cloud 的块数 */
/*A0*/	__le32		s_cld_files_count;	/* 迁移到 Cloud 的文件数 */

	__le32		s_upper_ratio;		/* 触发迁移的空间使用率 */
	__le32		s_max_unaccess;		/* 触发迁移的文件访问年龄 - 秒 */
	__le64		s_total_access;		/* 数据块的总访问次数 */
	__u32		s_pad1[78];		/* 填充到 512 字节 */
	__le32		s_blks_per_level[FMC_MAX_LEVELS];/* 约1K-每个访问级别的块数 */
	__le32		s_pad2[6];
};

struct hdd_sb_info {
	struct rw_semaphore	sbi_rwsem;	/* 读不变部分时, 不需加此锁;
						   处理可变分时,需先加此读锁, 再加个字锁; 
						   写超级块时,需写锁 */

	struct super_block	*sb;
	struct buffer_head	*hdd_bh;
	struct hdd_super_block	*hdd_sb;
	unsigned int		sb_block;	/* 超级块所在块号 */
	int			s_dirty;	/* 私有信息是否为脏 */

	unsigned int		mount_opt;	/* 自定义的挂载选项 */
	unsigned int		mount_state;	/* 挂载状态 */

	struct buffer_head	**group_desc;	/* 组描述符块的缓存头 */
	struct blockgroup_lock	*blkgrp_lock;	/* 组描述符锁 */
	/* 不变部分 */
	unsigned int		groups_count;	/* 组总数 */
	unsigned int		blocks_count;	/* 块总数 */
	unsigned int		inodes_count;	/* inode 总数 */
	unsigned int		gdt_blocks;	/* 组描述符所占块数 */
	unsigned int		itable_blocks;	/* 块组中 inode 的块数 */
	unsigned int		inode_size;	/* inode 大小 */	
	unsigned int		block_size;	/* 块大小 */
	unsigned int		log_blocksize;	/* 块大小 log */
	unsigned int		blks_per_group;	/* 块组中的 block 总数 */
	unsigned int		last_group_blks;/* 最后块组的总块数 */
	unsigned int		inodes_per_group;/* 块组中的 inode 数 */
	unsigned int		inodes_per_block;/* 块中的 inode 数 */
	unsigned int		desc_per_block;	/* 每个块中的组描述符个数 */
	unsigned int		desc_per_blk_bits;/* 每块中的组描述符 */
	unsigned int		addr_per_blk;	/* 每块中的地址个数 */
	unsigned int		root_ino_num;	/* 根 inode 号 */
	unsigned int		first_ino;	/* 首个非保留 inode 号 */
	unsigned int		grp_data_offset;/* 块组中用户数据块的偏移块数 */

	/* 变化部分 */
	struct percpu_counter	usr_blocks;	/* 用户已用的纯数据的块数 */
	struct percpu_counter	free_blks_count;/* 空闲块个数 */
	struct percpu_counter	free_inodes_count;/* 空闲 inode 个数 */
	struct percpu_counter	total_access;	/* 数据块的总访问次数 */
	struct percpu_counter	*blks_per_lvl;	/* 每访问级别中的块数 */
	struct percpu_counter	ssd_blks_count;	/* 迁移到 SSD 的块数 */

	struct mutex		ssd_mutex;	/* 控制访问 ssd_info */	
	struct ssd_sb_info	*ssd_info;	/* 所对应的 ssd 信息 */
	struct block_device	*ssd_bdev;	/* ssd 的块设备信息 */
	char			cld_name[16];	/* 云存储名字 */
	int			hdd_idx;	/* 在 ssd 中 uuid 数组的下标 */

	spinlock_t		cld_lock;	/* 控制访问云信息 */
	u64			cld_blks_count;	/* 迁移到 Cloud 的块数 */
	unsigned int		cld_files_count;/* 迁移到 Cloud 的文件数 */
	
	unsigned int		upper_ratio;	/* 触发迁移块的空间使用率 */
	unsigned int		max_unaccess;	/* 触发迁移文件的未访问年龄 - 秒 */
};

struct hdd_inode {
/*00*/	__le16		i_mode;			/* 文件类型和访问权限 */
	__le16		i_links_count;		/* 硬链接数 */
	__le32		i_flags;		/* 文件标志和迁移标志*/
	__le32		i_uid;			/* Owner Uid */
	__le32		i_gid;			/* Group Id */
/*10*/	__le64		i_size;			/* 文件长度 */
	__le32		i_blocks;		/* 块数 */
	__le32		i_access_count;		/* 访问统计信息 */
/*20*/	__le32		i_atime;		/* 访问时间 */
	__le32		i_ctime;		/* 上次修改 inode 时间 */
	__le32		i_mtime;		/* 修改时间 */
	__le32		i_dtime;		/* 删除时间 */
/*30*/	union {
	struct{
	__le64		i_cld_id;		/* 在云中的 ID */
	char		cld_info[72];		/* 在云中的信息 */
	}s_cloud;
	struct{
	__le32		i_ssd_blocks;		/* 在 ssd 中的块数 */
	__le16		i_pad;
	__le16		i_direct_bits;		/* 前12个块的位置标志 */
	__u8		i_direct_blks[HDD_NDIR_BLOCKS];/* 前12个块的访问计数 */
	__le32		i_block[HDD_N_BLOCKS];	/* 地址数组[15] */
	}s_hdd;
	}u;
};

struct hdd_inode_info {
	struct inode	vfs_inode;		/* vfs inode */
	__le32		i_data[15];		/* 文件内容地址 */
	unsigned int	i_flags;		/* 文件标志和迁移标志 */
	unsigned int	i_state;		/* inode 状态: STATE_NEW.. */
	unsigned int	i_dtime;		/* 删除时间 */
	unsigned int	i_block_group;		/* inode 所在块组号 */
	unsigned int	i_dir_start_lookup;	/* 开始查找的目录 */

	unsigned int	i_access_count;		/* 访问计数 */
	unsigned int	i_ssd_blocks;		/* 在 SSD 中的块数 */
	__u16		i_direct_bits;		/* 前12个块的位置标志 */
	__u16		i_pad;
	__u8		i_direct_blks[HDD_NDIR_BLOCKS];	/* 前12个块的访问计数 */
					
	struct mutex	truncate_mutex;		/* 用于序列化 hdd_truncate, hdd_getblock */
	rwlock_t	i_meta_lock;
	struct list_head i_orphan;		/* unlinked but open inodes */
};

struct hdd_group_desc {
	__le32		bg_block_bitmap;	/* 块位图的块号 */
	__le32		bg_inode_bitmap;	/* inode 位图的块号 */
	__le32		bg_inode_table;		/* inode 表的起始块号 */
	__le32		bg_free_blocks_count;	/* 空闲块数 */
	__le32		bg_free_inodes_count;	/* 空闲 inode 数 */
	__le32		bg_used_dirs_count;	/* 目录个数 */
	__le32		bg_reserved[2];		/* 保留 */
};

struct hdd_dir_entry {
	__le32		ino;			/* inode 号 */
	__le16		rec_len;		/* dentry 长度 */
	__u8		name_len;		/* 文件名实际长度 */
	__u8		file_type;		/* 文件类型*/
	char		name[HDD_NAME_LEN];	/* 255,文件名 */
};

typedef struct hdd_dir_entry hdd_dirent;

/* 取得块组锁 */
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

/* inode 在内存和磁盘上的位置 */
struct hdd_iloc {
	struct buffer_head *bh;
	unsigned long offset;
	unsigned long block_group;
};

/* 取得 hdd inode 内容*/
static inline struct hdd_inode *hdd_raw_inode(struct hdd_iloc *iloc)
{
	return (struct hdd_inode *) (iloc->bh->b_data + iloc->offset);
}

#define HDD_BLOCKS_PER_GROUP(s)	(HDD_SB(s)->blks_per_group)
#define HDD_DESC_PER_BLOCK(s)	(HDD_SB(s)->desc_per_block)
#define HDD_INODES_PER_GROUP(s)	(HDD_SB(s)->inodes_per_group)
#define HDD_DESC_PER_BLOCK_BITS(s) (HDD_SB(s)->desc_per_blk_bits)

/* 计算块组起始块号 */
static inline unsigned int
hdd_group_first_block_no(struct super_block *sb, unsigned long group_no)
{
	return group_no * (unsigned int)HDD_BLOCKS_PER_GROUP(sb) +
		le32_to_cpu(HDD_SB(sb)->hdd_sb->s_first_data_block);
}

/* 检查 ino 是否合法 */
static inline int hdd_valid_inum(struct super_block *sb, unsigned long ino)
{
	return  ino == HDD_ROOT_INO ||
	       (ino >= HDD_FIRST_INO &&
		ino <= le32_to_cpu(HDD_SB(sb)->hdd_sb->s_inodes_count));
}

/* 挂载选项 */
#define HDD_MOUNT_CHECK			0x00001	/* 装载时检查 */
#define HDD_MOUNT_DEBUG			0x00008	/* 一些调试信息 */

/* 清除, 设置, 测试挂载选项 */
#define clear_opt(o, opt)		o &= ~HDD_MOUNT_##opt
#define set_opt(o, opt)			o |= HDD_MOUNT_##opt
#define test_opt(sb, opt)		(HDD_SB(sb)->s_mount_opt & \
					HDD_MOUNT_##opt)

/* 哈希数目录索引 */
#define is_dx(dir) ((HDD_I(dir)->i_flags & HDD_INDEX_FL))
#define HDD_DIR_LINK_MAX(dir) (!is_dx(dir) && (dir)->i_nlink >= HDD_LINK_MAX)
#define HDD_DIR_LINK_EMPTY(dir) ((dir)->i_nlink == 2 || (dir)->i_nlink == 1)

/* hash info structure used by the directory hash */
struct dx_hash_info{
	u32		hash;		/* 哈希值 */
	u32		minor_hash;	/*  */
	int		hash_version;	/* 所用的哈希版本 */
	u32		*seed;		/* 自定义种子 */
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

/* 块分配相关 - balloc.c */
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

/* 文件同步操作 - fsync.c */
extern int hdd_sync_file (struct file *, struct dentry *, int);

/* 目录哈希操作 - hash.c */
extern int hddfs_dirhash(const char *name, int len, struct
			 dx_hash_info *hinfo);
/* inode 分配相关 - ialloc.c */
extern struct inode * hdd_new_inode (struct inode *, int);
extern void hdd_free_inode (struct inode *);
extern struct inode * hdd_orphan_get (struct super_block *, unsigned long);
extern unsigned long hdd_count_dirs (struct super_block *);
extern void hdd_check_inodes_bitmap (struct super_block *);
extern unsigned long hdd_count_free (struct buffer_head *, unsigned);

/* 对 inode 的操作 - inode.c */
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

/* 控制命令 - ioctl.c */
extern long hdd_ioctl(struct file *, unsigned int, unsigned long);

/* 孤儿文件 - namei.c */
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

	/* 错误与警告 - super.c */

#define hdd_std_error(sb, errno)				\
	do {							\
		if ((errno))					\
		    __hdd_std_error((sb), __func__, (errno));	\
	} while (0)

extern const struct address_space_operations hdd_aops;
/* 目录文件 操作函数表 - dir.c */
extern const struct file_operations hdd_dir_operations;
/* 文件操作函数表 - file.c */
extern const struct file_operations hdd_file_operations;
extern const struct inode_operations hdd_file_inode_operations;
/* inode 操作函数表 - namei.c */
extern const struct inode_operations hdd_dir_inode_operations;
extern const struct inode_operations hdd_special_inode_operations;
/* 符号链接操作函数表 - symlink.c */
extern const struct inode_operations hdd_symlink_inode_operations;
extern const struct inode_operations hdd_fast_symlink_inode_operations;


#endif	/*__LINUX_FS_FMC_HDD_H__*/
