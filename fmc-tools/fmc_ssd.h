/*
 * fmc_ssd.h
 */

#ifndef __FMC_SSD_H__
#define __FMC_SSD_H__

#include "fmc_tools.h"

#define SSD_SECTOR_SIZE		512		/* ������С */
#define SSD_SECTORS_PER_BLK	8		/* Ĭ�� 4K �� */
#define SSD_BLKS_PER_SEG	512		/* ���п���: 4KB => 2 MB  �� */
#define	SSD_SEGS_PER_SEC	256		/* ���ж���: 4KB => 512MB �� */
#define SSD_DEF_MAXRATIO	80		/* Ĭ�� ssd ���ʹ���� */
#define SSD_MAX_SECS		1024		/* ��� 1024 ��, �� 512 G */

struct ssd_global_vars {			/* Ĭ��ȫ�ֲ��� */
	u_int32_t       sector_size;		/* ������С */
	u_int32_t       start_sector;		/* ��ʼ sector */
	u_int64_t	total_sectors;		/* �� sector �� */

	u_int32_t       sectors_per_blk;	/* ÿ�� sector �� */
	u_int32_t	blk_kilo_size;		/* ��λ KB */
	u_int32_t       blks_per_seg;		/* �εĿ��� */
	u_int32_t       segs_per_sec;		/* sect �Ķ��� */

	u_int32_t	total_blks;		/* �ܿ��� */
	u_int32_t	total_segs;		/* �ܶ��� */
	u_int32_t	total_secs;		/* ������ */
	u_int32_t	last_sec;		/* ���1���Ķ��� */

	u_int32_t	upper_ratio;		/* ���ռ�ռ���� */

	int32_t         fd;			/* �豸�ļ������� */
	char		*device_name;		/* �豸�ļ��� */
};

struct ssd_sb_const {				/* ��������Ϣ���䲿�� */
/*00*/	__le32		s_magic;
	__le16		s_major_ver;
	__le16		s_minor_ver;

	__le32		s_log_sectorsize;	/* �ȳ�:9, �� log2(512) */
	__le32		s_log_blocksize;	/* �鳤, �� 4KB */
/*10*/	__le32		s_log_seg_size;		/* �γ� */
	__le32		s_log_sec_size;		/* ���� */

	__le32		s_log_sectors_per_blk;	/* ���������� log */
	__le32		s_log_blocks_per_seg;	/* ���п���:9, �� log(512) */
/*20*/	__le32		s_log_segs_per_sec;	/* ���ж���:8, �� log(256) */
	__le32		s_sit_segs_per_sec;	/* ���е� sit ����: 1 */

	__le32		s_block_count;		/* �ܿ��� */
	__le32		s_usr_blk_count;	/* �û��Ŀ��� */
/*30*/	__le32		s_seg0_blkaddr;		/* �׸��ε���ʼ���:1 */
	__le32		s_seg_count;		/* �ܶ���(���� boot ��) */
	__le32		s_sec_count;		/* ������(���� sb ��) */
	__le32		s_segs_last_sec;	/* ���һ���Ķ��� */
/*40*/	__u8		s_uuid[16];		/* �� UUID */
/*50*/	__u8		hdd_uuids[SSD_MAX_HDDS][16]; /* 32*16=512 B - �䶯���� */
	__le32		s_hdd_count;		/* ��Ӧ�� hdd ���� */
	__u32		s_pad[107];
};

struct ssd_sb_volatile {			/* ��������Ϣ�ױ䲿�� - 8 KB */
	__le32		sb_verion;		/* sb �汾�� */
	__le32		elapsed_time;		/* ����ʱ�� */
	__le32		free_seg_count;		/* �ܵĿ��ж��� */
	__le32		valid_blks;		/* �û���Ч�Ŀ��� */
	__le32		invalid_blk_count;	/* �û���Ч���� */
	__le32		pad;
	__le32		blks_per_level[FMC_MAX_LEVELS];	/* ÿ������� */
	__u8		free_segs[SSD_MAX_SECS];/* 1K-�����еĿ��ж��� */
	__le16		invalid_blks[SSD_MAX_SECS];/* 2K-��������Ч��(���256M) */
	__le32		free_blks[SSD_MAX_SECS];/* 4K-�����п��п��� */
};

/* ��״̬ */
#define SEG_FREE	0x0001
#define SEG_UPDATING	0x0002
#define SEG_USED	0x0004
#define SEG_NONEXIST	0x0008
struct ssd_sit {				/* ����Ϣ - 16 Bytes */
	__le16		stat;			/* ��״̬ */
	__le16		hdd_idx;		/* ������ hdd */
	__le32		invalid_blocks;		/* ��Ч���� */
	__le32		free_blocks;		/* ���п��� */
	__le32		mtime;			/* �޸�ʱ�� */
};
struct segs_info {				/* ���ж���Ϣ: 16 * 256 => 4 KB */
	struct ssd_sit sit[SSD_SEGS_PER_SEC];
};

struct block_info{				/* ����Ϣ - 8 Bytes */
	__le32		block_ino;		/* ���������ļ� */
	__le32		file_offset;		/* �����ļ��е�ƫ�� */
};
struct seg_blocks_info{				/* ���п���Ϣ: 8 * 512 = 4 KB */
	struct block_info blocks[SSD_BLKS_PER_SEG];
};

struct sec_trans_table {			/* 4 * 512 = 2 KB * 256 = 512 KB*/
	__le32	table[SSD_BLKS_PER_SEG];	
};

struct sec_trans_map {				/* ת��λͼ - 16 KB*/
	char map[SSD_BLKS_PER_SEG * SSD_SEGS_PER_SEC / 8];
};

#endif
