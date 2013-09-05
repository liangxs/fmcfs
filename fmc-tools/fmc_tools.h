/*
 * fmc_tools.h
 */

#ifndef __FMC_TOOLS_H__
#define __FMC_TOOLS_H__

#include <linux/types.h>
#include <endian.h>
#include <byteswap.h>

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define le16_to_cpu(x)	((__u16)(x))
#define le32_to_cpu(x)	((__u32)(x))
#define le64_to_cpu(x)	((__u64)(x))
#define cpu_to_le16(x)	((__u16)(x))
#define cpu_to_le32(x)	((__u32)(x))
#define cpu_to_le64(x)	((__u64)(x))
#elif __BYTE_ORDER == __BIG_ENDIAN
#define le16_to_cpu(x)	bswap_16(x)
#define le32_to_cpu(x)	bswap_32(x)
#define le64_to_cpu(x)	bswap_64(x)
#define cpu_to_le16(x)	bswap_16(x)
#define cpu_to_le32(x)	bswap_32(x)
#define cpu_to_le64(x)	bswap_64(x)
#endif

#define FMC_MIN_VOLUME_SIZE	204800000	/* ����Сֵ: 200 MB */

#define FMC_MAJOR_VERSION	1		/* �汾�� */
#define FMC_MINOR_VERSION	0

#define FMC_MAX_LEVELS		250		/* ��������Ϣ���� */

#define HDD_MAGIC		0xF3CF58DD	/* HDD �ļ�ϵͳħ�� */
#define SSD_MAGIC		0xF3CF555D	/* SSD �ļ�ϵͳħ�� */

#define SSD_MAX_HDDS		32		/* SSD �ɶ�Ӧ�� HDD �� */

#define HDD_NAME_LEN		255		/* �ļ�����󳤶� */
#define CLD_NAME_LEN		16		/* Cloud ������󳤶� */
static const char * const DEFAULT_CLOUD = "aliyun_oss";

#define HDD_FT_UNKNOWN		0		/* �ļ����� */
#define HDD_FT_REG_FILE		1
#define HDD_FT_DIR		2
#define HDD_FT_CHRDEV		3
#define HDD_FT_BLKDEV		4
#define HDD_FT_FIFO		5
#define HDD_FT_SOCK		6
#define HDD_FT_SYMLINK		7
#define HDD_FT_MAX		8


#endif
