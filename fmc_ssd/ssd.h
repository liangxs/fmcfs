/*
 * linux/fs/fmc/ssd.h
 *
 * Copyright (C) 2013 Liang Xuesen, <liangxuesen@gmail.com>
 * Beijing University of Posts and Telecommunications,
 * CPU center @ Tsinghua University.
 *
 */

#ifndef __LINUX_FS_FMC_SSD_H__
#define __LINUX_FS_FMC_SSD_H__

#include <linux/types.h>
#include <linux/page-flags.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/magic.h>

#include "../fmc_fs.h"
#include "../fmc_cld/cld.h"
#include "../fmc_hdd/hdd.h"

#define SSD_SECTOR_SIZE		512		/* 扇区大小 */
#define SSD_SECTORS_PER_BLK	8		/* 默认 4K 块 */
#define SSD_BLKS_PER_SEG	512		/* 段中块数: 4KB => 2 MB  段 */
#define	SSD_SEGS_PER_SEC	256		/* 区中段数: 4KB => 512MB 区 */
#define SSD_DEF_MAXRATIO	80		/* 默认 ssd 最大使用率 */
#define SSD_MAX_SECS		1024		/* 最多 1024 区, 即 512 G */

extern struct list_head ssd_sb_infos;
extern spinlock_t	ssd_sbi_lock;

struct ssd_sb_const {				/* 超级块信息不变部分 */
/*00*/	__le32		s_magic;
	__le16		s_major_ver;
	__le16		s_minor_ver;

	__le32		s_log_sectorsize;	/* 扇长:9, 即 log2(512) */
	__le32		s_log_blocksize;	/* 块长, 即 4KB */
/*10*/	__le32		s_log_seg_size;		/* 段长 */
	__le32		s_log_sec_size;		/* 区长 */

	__le32		s_log_sectors_per_blk;	/* 块中扇数的 log */
	__le32		s_log_blocks_per_seg;	/* 段中块数:9, 即 log(512) */
/*20*/	__le32		s_log_segs_per_sec;	/* 区中段数:8, 即 log(256) */
	__le32		s_sit_segs_per_sec;	/* 区中的 sit 段数: 1 */

	__le32		s_block_count;		/* 总块数 */
	__le32		s_usr_blk_count;	/* 用户的块数 */
/*30*/	__le32		s_seg0_blkaddr;		/* 首个段的起始块号:1 */
	__le32		s_seg_count;		/* 总段数(不计 boot 块) */
	__le32		s_sec_count;		/* 总区数(不计 sb 段) */
	__le32		s_segs_last_sec;	/* 最后一区的段数 */
/*40*/	__u8		s_uuid[16];		/* 卷 UUID */
/*50*/	__u8		hdd_uuids[SSD_MAX_HDDS][16]; /* 32*16=512 B - 变动较少 */
	__le32		s_hdd_count;		/* 对应的 hdd 个数 */
	__u32		s_pad[107];
};

struct ssd_sb_volatile {			/* 超级块信息易变部分 - 8 KB */
	__le32		sb_verion;		/* sb 版本号 */
	__le32		elapsed_time;		/* 挂载时间 */
	__le32		free_seg_count;		/* 总的空闲段数 */
	__le32		valid_blks;		/* 用户有效的块数 */
	__le32		invalid_blk_count;	/* 用户无效块数 */
	__le32		pad;
	__le32		blks_per_level[250];	/* 每级别块数 */
	__u8		free_segs[SSD_MAX_SECS];/* 1K-各区中的空闲段数 */
	__le16		invalid_blks[SSD_MAX_SECS];/* 2K-各区中无效块(最多256M) */
	__le32		free_blks[SSD_MAX_SECS];/* 4K-各区中空闲块数 */
};

#define SEG_FREE	0x0001			/* 段状态: 未使用 */
#define SEG_UPDATING	0x0002			/* 未满 */
#define SEG_USED	0x0004			/* 已满 */
#define SEG_NONEXIST	0x0008			/* 段不存在 */

struct ssd_sit {				/* 段信息 - 16 Bytes */
	__le16		stat;			/* 段状态 */
	__le16		hdd_idx;		/* 段所属 hdd */
	__le32		invalid_blocks;		/* 无效块数 */
	__le32		free_blocks;		/* 空闲块数 */
	__le32		mtime;			/* 修改时间 */
};
struct segs_info {				/* 区中段信息: 16 * 256 => 4 KB */
	struct ssd_sit sit[SSD_SEGS_PER_SEC];
};

struct block_info{				/* 块信息 - 8 Bytes */
	__le32		block_ino;		/* 块所属的文件 */
	__le32		file_offset;		/* 块在文件中的偏移 */
};
struct seg_blocks_info{				/* 段中块信息: 8 * 512 = 4 KB */
	struct block_info blocks[SSD_BLKS_PER_SEG];
};

struct sec_trans_table {			/* 4 * 512 = 2 KB * 256 = 512 KB*/
	__le32	table[SSD_BLKS_PER_SEG];	
};

struct sec_trans_map {				/* 转换位图 - 16 KB*/
	char map[SSD_BLKS_PER_SEG * SSD_SEGS_PER_SEC / 8];
};

struct ssd_sb_info {
	__u8			uuid[16];	/* ssd 分区的 UUID */
	atomic_t		refernce;	/* 当前引用此结构的个数 */
	struct super_block	*sb;		/* vfs 超级块 */
	struct block_device	*bdev;		/* 对应的块设备驱动 */
	struct list_head	sdi_list;	/* 所有 ssd 信息的链表 */

	struct buffer_head	*sbc_bh;	/* sbc 缓冲块 */
	struct ssd_sb_const	*sbc;		/* sbc */
	int			sbc_dirty;	/* sbc 更改标志 */

	struct buffer_head	*sbv_bh1;	/* sbv 缓冲块 */
	struct buffer_head	*sbv_bh2;	/* sbv 缓冲块 */
	struct ssd_sb_const	*sbv1;		/* sbv 的前 4K */
	struct ssd_sb_const	*sbv2;		/* sbv 的后 4K */
	int			sbv_dirty;	/* sbv 更改标志 */

	spinlock_t		hdd_count_lock;	/* 用于处理 HDD 个数 */
	int			hdd_count;	/* 有效 HDD 的个数 */
	spinlock_t		hdd_info_lock;	/* 用于处理 HDD 信息数组 */
	struct hdd_sb_info	*hdd_info[SSD_MAX_HDDS];/* 对应的 HDD 的私有信息 */

	unsigned long		s_blocksize;	/* 块大小 */
	unsigned char		s_blocksize_bits;/* 块大小 bit */

	unsigned int s_blocks_per_group;/* 块组中的块数 */
	unsigned int s_mount_opt;	/* 挂载选项 */

	unsigned int	s_mount_state;	/* 挂载状态 */

	struct percpu_counter s_freeblocks_counter;
	struct percpu_counter s_freeinodes_counter;
};

#endif /*__LINUX_FS_FMC_SSD_H__*/