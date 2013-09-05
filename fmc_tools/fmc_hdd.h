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

#define HDD_SECTOR_SIZE		512		/* ������С */
#define HDD_SECTORS_PER_BLK	8		/* Ĭ�� 4K �� */

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

struct hdd_global_vars {
	char		*cld_service;
	char		*ssd_device;
	char		*hdd_device;
	u_int32_t	log_block_size;		/* �鳤: 4096 * (2^#) */
	u_int32_t	block_size;
	u_int64_t	group_size;
	int32_t		ratio;
	int32_t		age;

	__u8		hdd_uuid[16];
	__u8		ssd_uuid[16];

	int32_t		fd;			/* �豸�ļ� */
	u_int32_t	sector_size;		/* �豸������С */
	u_int32_t	start_sector;		/* �豸��ʼ���� */
	u_int64_t	total_sectors;		/* �豸�������� */
	u_int32_t	total_blocks;		/* ���� */
	u_int32_t	total_groups;		/* ������ */
	u_int32_t	gdt_blocks;		/* ������������ռ���� */
	u_int32_t	itable_blocks;		/* inode ����ռ���� */
	u_int32_t	inodes_per_group;	/* ÿ�� inode �� */
	u_int32_t	blks_per_group;		/* ÿ����� */
	u_int32_t	blks_last_group;	/* ���һ��������ܿ��� */
};

struct hdd_super_block {
/*00*/	__le32		s_magic;		
	__le16		s_major_ver;		/* �ļ�ϵͳ�汾 */
	__le16		s_mimor_ver;
	__le32		s_groups_count;		/* group ���� */
	__le32		s_blocks_count;		/* block ���� */
/*10*/	__le32		s_inodes_count;		/* inode ���� */
	__le32		s_gdt_blocks;		/* ������������ռ���� */
	__le32		s_itable_blocks;	/* inode ����ռ���� */
	__le32		s_user_blocks;		/* �û��Ĵ����ݵĿ���(���Ǽ�ӿ�) */
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

struct hdd_group_desc {
	__le32		bg_block_bitmap;	/* ��λͼ�Ŀ�� */
	__le32		bg_inode_bitmap;	/* inode λͼ�Ŀ�� */
	__le32		bg_inode_table;		/* inode �����ʼ��� */
	__le32		bg_free_blocks_count;	/* ���п��� */
	__le32		bg_free_inodes_count;	/* ���� inode �� */
	__le32		bg_used_dirs_count;	/* Ŀ¼���� */
	__le32		bg_reserved[2];		/* ���� */
};

#define HDD_IF_ONHDD	0x00010000		/* ȫ�� HDD �� */
#define HDD_IF_ONBOTH	0x00020000		/* �� HDD �� SSD �� */
#define	HDD_IF_ONSSD	0x00040000		/* ȫ�� SSD �� */
#define HDD_IF_ONCLD	0x00080000		/* �� CLD �� */
#define HDD_IF_NOMIG	0x00100000		/* ��ֹǨ���ļ� */
#define HDD_IF_MUSTMIG	0x00200000		/* ����Ǩ���ļ� */
#define HDD_IF_NOFRAG	0x00400000		/* Ǩ�Ƶ� SSD ʱ���ֿ� */
#define HDD_IF_MIGING	0x00800000		/* �ļ����ڱ�Ǩ�Ƶ� CLD */

struct hdd_inode {
/*00*/	__le16		i_mode;			/* �ļ����ͺͷ���Ȩ�� */
	__le16		i_links_count;		/* Ӳ������ */
	__le32		i_flags;		/* ��־, �ļ�Ǩ����Ϣ*/
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

struct hdd_dir_entry {
	__le32		ino;			/* inode �� */
	__le16		rec_len;		/* dentry ���� */
	__u8		name_len;		/* �ļ���ʵ�ʳ��� */
	__u8		file_type;		/* �ļ�����*/
	char		name[HDD_NAME_LEN];	/* 255,�ļ��� */
};


#endif
