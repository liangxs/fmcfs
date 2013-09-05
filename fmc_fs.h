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

/* ioctl ���� */
#define	HDD_IOC_GETFLAGS		FS_IOC_GETFLAGS
#define	HDD_IOC_SETFLAGS		FS_IOC_SETFLAGS

/* ����ѡ�� */
struct hdd_mount_options {
	unsigned long	s_mount_opt;
	unsigned long	s_commit_interval;
};


/* ����ѡ�� */
#define HDD_MOUNT_CHECK			0x00001	/* װ��ʱ��� */
#define HDD_MOUNT_DEBUG			0x00008	/* һЩ������Ϣ */

/* ���, ����, ���Թ���ѡ�� */
#define clear_opt(o, opt)		o &= ~HDD_MOUNT_##opt
#define set_opt(o, opt)			o |= HDD_MOUNT_##opt
#define test_opt(sb, opt)		(HDD_SB(sb)->s_mount_opt & \
					HDD_MOUNT_##opt)

#ifdef __KERNEL__
#include <linux/fmc_hdd_i.h>
#include <linux/fmc_hdd_sb.h>

/* �� vfs sb ��ȡ����˽����Ϣ */
static inline struct hdd_sb_info * HDD_SB(struct super_block *sb) {
	return sb->s_fs_info;
}
/* �� vfs inode ��ȡ��˽����Ϣ */
static inline struct hdd_inode_info *HDD_I(struct inode *inode) {
	return container_of(inode, struct hdd_inode_info, vfs_inode);
}

/* ��� ino �Ƿ�Ϸ� */
static inline int hdd_valid_inum(struct super_block *sb, unsigned long ino)
{
	return  ino == HDD_ROOT_INO || ino == HDD_JOURNAL_INO 
		||(ino >= HDD_FIRST_INO(sb) &&
		ino <= le32_to_cpu(HDD_SB(sb)->s_es->s_inodes_count));
}
#else
#define HDD_SB(sb)	(sb)
#endif

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

#define NEXT_ORPHAN(inode) HDD_I(inode)->i_dtime


/* ��ϣ��Ŀ¼���� */
#define is_dx(dir) ((HDD_I(dir)->i_flags & HDD_INDEX_FL))
#define HDD_DIR_LINK_MAX(dir) (!is_dx(dir) && (dir)->i_nlink >= HDD_LINK_MAX)
#define HDD_DIR_LINK_EMPTY(dir) ((dir)->i_nlink == 2 || (dir)->i_nlink == 1)

#ifdef __KERNEL__

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

/* inode ���ڴ�ʹ����ϵ�λ�� */
struct hdd_iloc {
	struct buffer_head *bh;
	unsigned long offset;
	unsigned long block_group;
};
/* ȡ�� hdd inode ����*/
static inline struct hdd_inode *hdd_raw_inode(struct hdd_iloc *iloc) {
	return (struct hdd_inode *) (iloc->bh->b_data + iloc->offset);
}

/* Ŀ¼�ļ���˽����Ϣ
 * This structure is stuffed into the struct file's private_data field
 * for directories.  It is where we put information so that we can do
 * readdir operations in hash tree order. */
struct dir_private_info {
	struct rb_root	root;		/* ������� */
	struct rb_node	*curr_node;	/* ��ǰ�ڵ� */
	struct fname	*extra_fname;
	loff_t		last_pos;
	__u32		curr_hash;
	__u32		curr_minor_hash;
	__u32		next_hash;
};

/* �������ĵ�һ����� */
static inline hdd_fsblk_t hdd_group_first_block_no(
	struct super_block *sb, unsigned long group_no)
{
	return group_no * (hdd_fsblk_t)HDD_BLOCKS_PER_GROUP(sb) +
		le32_to_cpu(HDD_SB(sb)->s_es->s_first_data_block);
}

/* Error return code only used by dx_probe() and its callers. */
#define ERR_BAD_DX_DIR	-75000


/* also in <linux/kernel.h> ,are duplicated here. */
# define NORET_TYPE    /**/
# define ATTRIB_NORET  __attribute__((noreturn))
# define NORET_AND     noreturn,


/* �����뾯�� - super.c */

extern void hdd_update_dynamic_rev (struct super_block *sb);

/* ��׼���� */
#define hdd_std_error(sb, errno)				\
	do {							\
		if ((errno))					\
		    __hdd_std_error((sb), __func__, (errno));	\
	} while (0)


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

#endif	/* __KERNEL__ */


#endif /* __LINUX_FS_FMC_FMC_FS_H__ */