/*
 * fmc_hdd.h 
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * @ Beijing University of Posts and Telecommunications,
 * @ CPU Center of Tsinghua University.
 *
 * This program can be redistributed under the terms of the GNU Public License.
 */

#ifndef __FMC_HDD_H__
#define __FMC_HDD_H__

#include "fmc_tools.h"

#define HDD_SECTOR_SIZE		512		/* 扇区大小 */
#define HDD_SECTORS_PER_BLK	8		/* 默认 4K 块 */

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

struct hdd_global_vars {
	char		*cld_service;
	char		*ssd_device;
	char		*hdd_device;
	u_int32_t	log_block_size;		/* 块长: 4096 * (2^#) */
	u_int32_t	block_size;
	u_int64_t	group_size;
	int32_t		ratio;
	int32_t		age;

	__u8		hdd_uuid[16];
	__u8		ssd_uuid[16];

	int32_t		fd;			/* 设备文件 */
	u_int32_t	sector_size;		/* 设备扇区大小 */
	u_int32_t	start_sector;		/* 设备起始扇区 */
	u_int64_t	total_sectors;		/* 设备扇区个数 */
	u_int32_t	total_blocks;		/* 块数 */
	u_int32_t	total_groups;		/* 块组数 */
	u_int32_t	gdt_blocks;		/* 组描述符表所占块数 */
	u_int32_t	itable_blocks;		/* inode 表所占块数 */
	u_int32_t	inodes_per_group;	/* 每组 inode 数 */
	u_int32_t	blks_per_group;		/* 每组块数 */
	u_int32_t	blks_last_group;	/* 最后一个块组的总块数 */
};

struct hdd_super_block {
/*00*/	__le32		s_magic;		
	__le16		s_major_ver;		/* 文件系统版本 */
	__le16		s_mimor_ver;
	__le32		s_groups_count;		/* group 总数 */
	__le32		s_blocks_count;		/* block 总数 */
/*10*/	__le32		s_inodes_count;		/* inode 总数 */
	__le32		s_gdt_blocks;		/* 组描述符表所占块数 */
	__le32		s_itable_blocks;	/* inode 表所占块数 */
	__le32		s_user_blocks;		/* 用户的纯数据的块数(不记间接块) */
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

struct hdd_group_desc {
	__le32		bg_block_bitmap;	/* 块位图的块号 */
	__le32		bg_inode_bitmap;	/* inode 位图的块号 */
	__le32		bg_inode_table;		/* inode 表的起始块号 */
	__le32		bg_free_blocks_count;	/* 空闲块数 */
	__le32		bg_free_inodes_count;	/* 空闲 inode 数 */
	__le32		bg_used_dirs_count;	/* 目录个数 */
	__le32		bg_reserved[2];		/* 保留 */
};

#define HDD_IF_ONHDD	0x00010000		/* 全在 HDD 上 */
#define HDD_IF_ONBOTH	0x00020000		/* 在 HDD 和 SSD 上 */
#define	HDD_IF_ONSSD	0x00040000		/* 全在 SSD 上 */
#define HDD_IF_ONCLD	0x00080000		/* 在 CLD 上 */
#define HDD_IF_NOMIG	0x00100000		/* 禁止迁移文件 */
#define HDD_IF_MUSTMIG	0x00200000		/* 必须迁移文件 */
#define HDD_IF_NOFRAG	0x00400000		/* 迁移到 SSD 时不分块 */
#define HDD_IF_MIGING	0x00800000		/* 文件正在被迁移到 CLD */

struct hdd_inode {
/*00*/	__le16		i_mode;			/* 文件类型和访问权限 */
	__le16		i_links_count;		/* 硬链接数 */
	__le32		i_flags;		/* 标志, 文件迁移信息*/
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

struct hdd_dir_entry {
	__le32		ino;			/* inode 号 */
	__le16		rec_len;		/* dentry 长度 */
	__u8		name_len;		/* 文件名实际长度 */
	__u8		file_type;		/* 文件类型*/
	char		name[HDD_NAME_LEN];	/* 255,文件名 */
};


#endif
