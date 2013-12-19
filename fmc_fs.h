/*
 * linux/fs/fmc/fmc_fs.h
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * Beijing University of Posts and Telecommunications,
 * CPU Center @ Tsinghua University.
 * 
 * This program can be redistributed under the terms of the GNU Public License.
 */

#ifndef __LINUX_FS_FMC_FMC_FS_H__
#define __LINUX_FS_FMC_FMC_FS_H__

#include <linux/types.h>
#include <linux/fs.h>

/* ���������Ϣ */
#define FMCFS_DEBUG

#ifdef  FMCFS_DEBUG
#define fmc_debug(f, a...)				\
	do {						\
	    printk (KERN_DEBUG "fmc-fs DEBUG (%s, %d): %s:",\
	    __FILE__, __LINE__, __func__);		\
	    printk (KERN_DEBUG f, ## a);		\
	} while (0)
#else
#define fmc_debug(f, a...)	do {} while (0)
#endif

#define FMC_MAJOR_VERSION	1		/* �汾�� */
#define FMC_MINOR_VERSION	0

#define FMC_MAX_LEVELS		250		/* ������Ϣ�ȼ� */

#define FMC_MIN_VOLUME_SIZE	204800000	/* ����Сֵ: 200 MB */

#define HDD_MAGIC		0xF3CF58DD	/* HDD �ļ�ϵͳħ�� */
#define SSD_MAGIC		0xF3CF555D	/* SSD �ļ�ϵͳħ�� */

#define SSD_MAX_HDDS		32		/* SSD �ɶ�Ӧ�� HDD �� */

#define HDD_NAME_LEN		255		/* �ļ�����󳤶� */
#define CLD_NAME_LEN		16		/* Cloud ������󳤶� */
static const char * const DEFAULT_CLOUD = "aliyun_oss";

#define BLOCK_ON_HDD		0		/* ���� HDD �� */
#define BLOCK_ON_SSD		1		/* ���� SSD �� */

/*********************************************/

/* ioctl ���� */
#define	HDD_IOC_GETFLAGS		FS_IOC_GETFLAGS
#define	HDD_IOC_SETFLAGS		FS_IOC_SETFLAGS

/* ����ѡ�� */
struct hdd_mount_options {
	unsigned long	s_mount_opt;
	unsigned long	s_commit_interval;
};

/* Ŀ¼��߽�����
 * HDD_DIR_PAD defines the directory entries boundaries
 * NOTE: It must be a multiple of 4  */
#define HDD_DIR_PAD			4
#define HDD_DIR_ROUND			(HDD_DIR_PAD - 1) /* 3 */
#define HDD_DIR_REC_LEN(name_len)	(((name_len) + 8 + HDD_DIR_ROUND) & \
					 ~HDD_DIR_ROUND)
#define HDD_MAX_REC_LEN			((1<<16)-1)

/* �Ѵ��̸�ʽĿ¼���ת��Ϊ cpu ��ʽ */
static inline unsigned hdd_rec_len_from_disk(__le16 dlen) {
	unsigned len = le16_to_cpu(dlen); /* ֱ��ת�� */

	if (len == HDD_MAX_REC_LEN)	/* ֵΪ 65535 */
		return 1 << 16;
	return len;
}
/* ��Ŀ¼���ת��Ϊ���̸�ʽ */
static inline __le16 hdd_rec_len_to_disk(unsigned len) {
	if (len == (1 << 16))
		return cpu_to_le16(HDD_MAX_REC_LEN);
	else if (len > (1 << 16))
		BUG();
	return cpu_to_le16(len);
}

//#define NEXT_ORPHAN(inode) HDD_I(inode)->i_dtime


/* Ŀ¼�ļ���˽����Ϣ
 * This structure is stuffed into the struct file's private_data field
 * for directories.  It is where we put information so that we can do
 * readdir operations in hash tree order. */
struct dir_private_info {
	struct rb_root	root;		/* ������� */
	struct rb_node	*curr_node;	/* ��ǰ�ڵ� */
	//struct fname	*extra_fname;
	loff_t		last_pos;
	__u32		curr_hash;
	__u32		curr_minor_hash;
	__u32		next_hash;
};

/* Error return code only used by dx_probe() and its callers. */
#define ERR_BAD_DX_DIR	-75000

/* also in <linux/kernel.h> ,are duplicated here. */
# define NORET_TYPE    /**/
# define ATTRIB_NORET  __attribute__((noreturn))
# define NORET_AND     noreturn,

#endif /* __LINUX_FS_FMC_FMC_FS_H__ */
