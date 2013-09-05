/*
 * fmc_hdd.c - Format a hdd volume to a fmc filesystem.
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * @ Beijing University of Posts and Telecommunications,
 * @ CPU Center of Tsinghua University.
 *
 * This program can be redistributed under the terms of the GNU Public License.
 */

/* Usage: mkfs.fmc_hdd [options] device
 *
 * eg: mkfs -t fmc_hdd [-b 4] [-c aliyun_oss] [-s /dev/sdb1] /dev/sda10
 *	-b: blocksize, can be 4 KB.
 *	-c: cloud service name
 *	-s: ssd device name
 *	-r: upper limitation ratio, used for trigging migration
 *	-a: file age (second), used to trigging migration
 *	-f: ����Ǩ�Ƶ��ļ�����, ��ʵ��
 *	-w: ��Ҫ����Ǩ�Ƶ� SSD ���ļ�����, ��ͼ��, ��ʵ��
 *	device: hdd device name
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <mntent.h>
#include <inttypes.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <linux/hdreg.h>
#include <time.h>
#include <linux/fs.h>
#include <uuid/uuid.h>

#include "fmc_hdd.h"
#include "fmc_ssd.h"

struct hdd_global_vars	hdd_vars;
struct hdd_super_block	hdd_sb;
struct ssd_sb_const	ssd_sb;

/* ��� mkfs.fmc_hdd �����÷� */
static void hdd_usage(void)
{
	fprintf(stderr, "Usage: fmc_hdd [options] device\n");
	fprintf(stderr, "[options]\n");
	fprintf(stderr, "-a file age(second) to trigging migration [default:0]\n");
	fprintf(stderr, "-b blocksize, can be 4 [default:4]KB\n");
	fprintf(stderr, "-c cloud service name [default:NULL]\n");
	fprintf(stderr, "-s ssd device name [default:NULL]\n");
	fprintf(stderr, "-r upper used ratio [default:90]\n\n");

	exit(1);
}

/* ��ʼ��ȫ�ֲ��� */
static void hdd_init_vars(){

	memset(&hdd_vars, '\0', sizeof(hdd_vars));

	hdd_vars.block_size = 1 << hdd_vars.log_block_size << 12;

	hdd_vars.ratio = DEF_HDD_MAXRATIO;
	hdd_vars.age = DEF_HDD_MAXAGE;

	uuid_generate(hdd_vars.hdd_uuid);

	hdd_vars.sector_size = HDD_SECTOR_SIZE;		/* �豸������С */

	hdd_vars.inodes_per_group = 8 << 10; /* �̶�Ϊ 8K �� inode */
	hdd_vars.itable_blocks = HDD_INODE_SIZE * hdd_vars.inodes_per_group
		/ hdd_vars.block_size;
}

/* ��������ѡ�� */
static void hdd_parse_options(int argc, char *argv[])
{
	static const char *option_str = "a:b:c:s:r:";
	int32_t option = 0;
	int32_t tmp = 0;

	while ((option = getopt(argc, argv, option_str)) != EOF)
		switch (option) {
		case 'a':/* ���δ����ʱ�� */
			hdd_vars.age = atoi(optarg);
			if (hdd_vars.age < 3600)
				hdd_vars.age = 3600;/* ��С���� 1 Сʱ */
			break;
		case 'b':/* �鳤 */
			tmp = atoi(optarg);
			if (tmp != 4)
				hdd_usage();
			break;
		case 'c':/* �Ʒ������� */
			if (strlen(optarg) >= CLD_NAME_LEN) {
				printf("Error: Cloud name is too long!\n");
				hdd_usage();
			}			
			hdd_vars.cld_service = strdup(optarg);
			break;
		case 's':/* ssd�豸���� */
			hdd_vars.ssd_device = strdup(optarg);
			break;
		case 'r':/* ���ռ�ʹ���� */
			hdd_vars.ratio = atoi(optarg);
			if (hdd_vars.ratio < 10 || hdd_vars.ratio > 99)
				hdd_vars.ratio = DEF_HDD_MAXRATIO;
			break;
		default:
			printf("Error: Unknown option %c\n", option);
			hdd_usage();
			break;
		}		
	
	if (optind + 1 != argc) {
		printf("Error: HDD Device is not specified.\n");
		hdd_usage();
		exit(1);
	}
	hdd_vars.hdd_device = argv[optind];

	printf("Info: Max file unacess age: %d seconds\n", hdd_vars.age);			
	if (hdd_vars.ssd_device != NULL)
		printf("Info: SSD device: %s\n", hdd_vars.ssd_device);
	if (hdd_vars.cld_service != NULL)
		printf("Info: Cloud service: %s\n", hdd_vars.cld_service);
	printf("Info: HDD's max used space: %d%%\n", hdd_vars.ratio);
}

/* ����豸�Ƿ񱻹��� */
static int device_is_mounted(char *device_name)
{
	FILE *file;
	struct mntent *mnt;

	if ((file = setmntent(MOUNTED, "r")) == NULL)
		return 0;

	while ((mnt = getmntent(file)) != NULL) {
		if (!strcmp(device_name, mnt->mnt_fsname)) {
			printf("Info: %s is already mounted\n", device_name);
			return -1;
		}
	}
	endmntent(file);

	return 0;
}

/* ��ȡ�豸��Ϣ */
static int hdd_get_device_info(void)
{
	int32_t fd = 0;
	int32_t sector_size = 0;
	struct stat stat_buf;
	struct hd_geometry geom;

	/* ���豸�ļ� */
	fd = open(hdd_vars.hdd_device, O_RDWR);
	if (fd < 0) {
		printf("\n\tError: Failed to open the HDD device: %s!\n",
			hdd_vars.hdd_device);
		return -1;
	}
	hdd_vars.fd = fd;

	/* ��ȡ�ļ�״̬ */
	if (fstat(fd, &stat_buf) < 0 ) {
		printf("\n\tError: Failed to get the HDD device stat: %s!\n",
			hdd_vars.hdd_device);
		return -1;
	}

	/* ������������ */
	if (S_ISREG(stat_buf.st_mode)) {	/* �����ļ� */
		hdd_vars.total_sectors = stat_buf.st_size / 512;
	}else if (S_ISBLK(stat_buf.st_mode)) {	/* ���豸�ļ� */
		if (ioctl(fd, BLKSSZGET, &sector_size) < 0 ) /* ��ȡ������С */
			printf("\n\tError: Cannot get the HDD sector size! \
			       Using the default Sector Size\n");

		/* ��ȡ�������� */
		if (ioctl(fd, BLKGETSIZE, &hdd_vars.total_sectors) < 0) {
			printf("\n\tError: Cannot get the HDD device size\n");
			return -1;
		}

		/* ��ȡ�豸���� */
		if (ioctl(fd, HDIO_GETGEO, &geom) < 0) {
			printf("\n\tError: Cannot get the HDD device geometry\n");
			return -1;
		}
		hdd_vars.start_sector = geom.start; /* ��ʼ���� */
	}else {
		printf("\n\n\tError: HDD volume type is not supported!\n");
		return -1;
	}

	hdd_vars.total_blocks = (hdd_vars.total_sectors >> 3)
				>> hdd_vars.log_block_size;

	printf("Info: HDD sector size = %u bytes\n", hdd_vars.sector_size);
	printf("Info: HDD total sectors = %"PRIu64" (in 512bytes)\n",
		hdd_vars.total_sectors);
	printf("Info: Block size: %d KB\n", 
		(1 << hdd_vars.log_block_size) << 2);
	printf("Info: HDD total blocks = %u\n", hdd_vars.total_blocks);

	/* �ж��豸�����Ƿ񹻴� */
	if (hdd_vars.total_sectors <(FMC_MIN_VOLUME_SIZE / HDD_SECTOR_SIZE)) {
		printf("Error: Min volume size supported is %d Bytes\n",
			FMC_MIN_VOLUME_SIZE);
		return -1;
	}

	return 0;
}

/* ��ȡ ssd �� UUID, ������ hdd �� UUID */
static int get_and_set_uuid(void)
{
	int fd = -1;
	int idx = 0;
	__u8 zerouuid[16] = {'\0'};
	struct stat stat_buf;

	if(hdd_vars.ssd_device == NULL)	/* ��ʹ�� SSD */
		return 0;

	fd = open(hdd_vars.ssd_device, O_RDWR);/* �� ssd �豸�ļ� */
	if (fd < 0) {
		printf("\n\tError: Cannot open SSD device: %s\n", 
			hdd_vars.ssd_device);
		return -1;
	}

	if (fstat(fd, &stat_buf) < 0) {	/* ��ȡ�豸״̬ */
		printf("\n\tError: Failed to get the SSD stat: %s!\n",
			hdd_vars.ssd_device);
		return -1;
	}
	
	/* �ж��豸����: �����ļ�����豸 */
	if(S_ISREG(stat_buf.st_mode) || S_ISBLK(stat_buf.st_mode)) {

		/* �����׸����������ݿ� */
		if (-1 == lseek(fd, hdd_vars.block_size, SEEK_SET)){
			printf("\n\tError: cannot lseek SSD: %s\n",
				hdd_vars.ssd_device);
			return -1;
		}
		
		if (read(fd, &ssd_sb, sizeof(struct ssd_sb_const))
		!=  sizeof(struct ssd_sb_const)){
			printf("\n\tError: cannot read SSD: %s\n",
				hdd_vars.ssd_device);
			return -1;
		}

		/* �ж��Ƿ�Ϊ fmc_ssd �����豸 */
		if (le32_to_cpu(ssd_sb.s_magic) != SSD_MAGIC) {
			printf("\n\tError: %s is not a fmc_ssd volume!\n",
				hdd_vars.ssd_device);
			return -1;
		}
		/* �ж� SSD �� HDD �Ŀ��С�Ƿ���ͬ */
		if (1 << le32_to_cpu(ssd_sb.s_log_blocksize)
		!= hdd_vars.block_size) {
			printf("\n\tError: %s block size neq hdd block size!\n",
				hdd_vars.ssd_device);
			return -1;
		}
		/* �ж� SSD ���Ƿ��п��� HDD λ�� */
		if (ssd_sb.s_hdd_count >= SSD_MAX_HDDS) {
			printf("\nError: ssd %s already has %d hdd volumes!\n",
				hdd_vars.ssd_device, SSD_MAX_HDDS);
			return -1;
		}

		/* ��¼Ŀ�� SSD �� UUID */
		memcpy(hdd_vars.ssd_uuid, ssd_sb.s_uuid, 
			sizeof(hdd_vars.ssd_uuid));

		/* �� ssd δ����, ����Ӵ� hdd �� uuid ��Ϣ */
		if (!device_is_mounted(hdd_vars.ssd_device)) {
			ssd_sb.s_hdd_count = 
				le32_to_cpu(cpu_to_le32(ssd_sb.s_hdd_count)+1);

			/* ���ҿ�λ */
			for (idx = 0; idx < SSD_MAX_HDDS; ++idx)
				if (memcmp(ssd_sb.hdd_uuids[idx], zerouuid, 
					sizeof(zerouuid)) == 0)
				break;
			
			if (idx < SSD_MAX_HDDS) {
				memcpy(ssd_sb.hdd_uuids[idx], hdd_vars.hdd_uuid,
					sizeof(hdd_vars.hdd_uuid));
				hdd_sb.s_hdd_idx = cpu_to_le32(idx);
			}

			/* д�� SSD �� */
			if (write(fd, &ssd_sb, sizeof(struct ssd_sb_const))
				!=  sizeof(struct ssd_sb_const)){
				printf("\n\tError: cannot write SSD: %s\n",
						hdd_vars.ssd_device);
					return -1;
			}
		}
		
		close(fd);

	} else {
		printf("\n\tError: SSD volume type is not supported: %s!\n", 
			hdd_vars.ssd_device);
		return -1;
	}

	return 0;
}

static void calc_groups()
{
	u_int32_t max_groups = 0;
	u_int32_t groups = 0;
	u_int32_t gdt_blocks = 0;	/* ��������Ĵ�С */
	u_int32_t blocks_per_group = 0;
	u_int32_t blks_of_last_group;

	/* �ȼ������Ŀ��ܿ������ */
	max_groups = hdd_vars.total_blocks
		   / (1+1+1+1+ 8*1024*HDD_INODE_SIZE/hdd_vars.block_size
		       + hdd_vars.block_size * 8);
	max_groups ++;

	/* ���� groups ����Ѹ��� */
	if (max_groups < 5)
		max_groups = 5;
	for(groups = max_groups - 4; groups <= max_groups; ++groups) {
		/* gdt ����ȡ������ */
		gdt_blocks = (groups * 32 + hdd_vars.block_size - 1)
			   / hdd_vars.block_size;
		/* ÿ����Ŀ��� */
		blocks_per_group = 1+gdt_blocks+1+1+hdd_vars.itable_blocks
				 + (hdd_vars.block_size * 8);
		/* �ɴ˿������õ����ܿ����ճ���ʵ���ܿ��� */
		if (groups * blocks_per_group >= hdd_vars.total_blocks - 1)
			break;
	}

	/* ���һ�������ʵ�ʿ��� */
	blks_of_last_group = hdd_vars.total_blocks - 1 /* boot �� */
			   - (groups-1) * blocks_per_group;
	hdd_vars.blks_last_group = blks_of_last_group;
	/* ���һ�������������ݲ�������256��(1MB), �򲻼�֮ */
	if (blks_of_last_group < 3+gdt_blocks+hdd_vars.itable_blocks + 256){
		groups--;
		gdt_blocks = (groups * 32 + hdd_vars.block_size - 1)
			   / hdd_vars.block_size;

		blocks_per_group = 3 + gdt_blocks + hdd_vars.itable_blocks
			         + (hdd_vars.block_size * 8);

		/* �����ܿ��� */
		hdd_vars.total_blocks = groups * blocks_per_group + 1;
		hdd_vars.blks_last_group = blocks_per_group;
	} else {/* ʹ������Ŀ������ݿ�ĸ���Ϊ 8 �������� */
		int data_blks = blks_of_last_group - 
			(3+gdt_blocks+hdd_vars.itable_blocks);
		hdd_vars.total_blocks -= data_blks % 8;
		hdd_vars.blks_last_group -= data_blks % 8
	}

	hdd_vars.total_groups = groups;
	hdd_vars.gdt_blocks = gdt_blocks;
	hdd_vars.blks_per_group = blocks_per_group;
	hdd_vars.group_size = blocks_per_group * hdd_vars.block_size;
}

/* ��ʼ��������ṹ */
static int hdd_prepare_sb()
{
	time_t now = time(0);

	memset(&hdd_sb, '\0', sizeof(struct hdd_super_block));
	hdd_sb.s_magic		= cpu_to_le32(HDD_MAGIC);
	hdd_sb.s_major_ver	= cpu_to_le16(FMC_MAJOR_VERSION);
	hdd_sb.s_mimor_ver	= cpu_to_le16(FMC_MINOR_VERSION);
	hdd_sb.s_block_size	= cpu_to_le32(hdd_vars.block_size);
	hdd_sb.s_log_block_size	= cpu_to_le32(hdd_vars.log_block_size);
	hdd_sb.s_inode_size	= cpu_to_le32(HDD_INODE_SIZE);
	hdd_sb.s_first_data_block=cpu_to_le32(1);
	hdd_sb.s_block_group_nr	= cpu_to_le32(0);
	hdd_sb.s_inodes_per_group = cpu_to_le32(8 << 10); /* �̶� 8K �� inodes */
	hdd_sb.s_first_ino	= cpu_to_le32(HDD_FIRST_INO);

	calc_groups();
	hdd_sb.s_groups_count	= cpu_to_le32(hdd_vars.total_groups);
	hdd_sb.s_blocks_count	= cpu_to_le32(hdd_vars.total_blocks);
	hdd_sb.s_inodes_count	= cpu_to_le32((8 << 10)*hdd_vars.total_groups);
	hdd_sb.s_gdt_blocks	= cpu_to_le32(hdd_vars.gdt_blocks);
	hdd_sb.s_itable_blocks	= cpu_to_le32(hdd_vars.itable_blocks);

	hdd_sb.s_user_blocks = cpu_to_le32(2);/* ��Ŀ¼�ļ������ݿ��� */
	hdd_sb.s_free_blocks_count=cpu_to_le32(hdd_vars.total_blocks - 1/*boot*/
		- hdd_vars.total_groups * (3 + hdd_vars.gdt_blocks
		+ hdd_vars.itable_blocks) - 2/* ��Ŀ¼�ļ���2�� */);

	hdd_sb.s_free_inodes_count=cpu_to_le32((8 << 10)*hdd_vars.total_groups
		- HDD_FIRST_INO);

	hdd_sb.s_blocks_per_group= cpu_to_le32(hdd_vars.blks_per_group);
	hdd_sb.s_last_group_blocks=cpu_to_le32(hdd_vars.blks_last_group);
	hdd_sb.s_wtime		= cpu_to_le32(0);
	hdd_sb.s_mtime		= cpu_to_le32(0);
	hdd_sb.s_mkfs_time	= cpu_to_le32(now);
	hdd_sb.s_state		= cpu_to_le32(HDD_VALID_FS);

	memcpy(hdd_sb.s_uuid, hdd_vars.hdd_uuid, 16);
	memcpy(hdd_sb.s_ssd_uuid, hdd_vars.ssd_uuid, 16);

	if (NULL != hdd_vars.ssd_device)
		strncpy(hdd_sb.s_ssd_name, hdd_vars.ssd_device,/* ssd �豸�� */
		sizeof(hdd_sb.s_ssd_name));
	hdd_sb.s_hdd_idx = cpu_to_le32(-1);

	if (NULL != hdd_vars.cld_service)
		strncpy(hdd_sb.s_cld_name, hdd_vars.cld_service,/* cloud �� */
		sizeof(hdd_sb.s_cld_name));

	hdd_sb.s_upper_ratio	= cpu_to_le32(hdd_vars.ratio);	/* ���ռ�ʹ���� */
	hdd_sb.s_max_unaccess	= cpu_to_le32(hdd_vars.age);	/* �ļ��������� - �� */

	return 0;
}

/* �޼��豸 */
static int hdd_trim_device(void)
{
	unsigned long long range[2];
	struct stat stat_buf;

	range[0] = 0;
	range[1] = hdd_vars.total_blocks * HDD_SECTORS_PER_BLK * HDD_SECTOR_SIZE;

	/* ��ȡ�豸�ļ�״̬ */
	if (fstat(hdd_vars.fd, &stat_buf) < 0 ) {
		printf("\n\tError: Failed to get the device stat!\n");
		return -1;
	}

	if (S_ISREG(stat_buf.st_mode))
		return 0;
	else if (S_ISBLK(stat_buf.st_mode)) {
		/* �����豸�Ķ������� */
		if (ioctl(hdd_vars.fd, BLKDISCARD, &range) < 0)
			printf("Info: This device doesn't support TRIM\n");
	} else
		return -1;

	return 0;
}

/*  ��ʼ��λͼ�� inode table */
static int hdd_init_bmap_imap_itable(void)
{
	u_int32_t g_idx = 0, i_idx = 0, b_idx = 0;
	u_int64_t g_size = hdd_vars.group_size;
	u_int64_t offset = hdd_vars.block_size * (2 + hdd_vars.gdt_blocks);
	char buf[HDD_SECTORS_PER_BLK * HDD_SECTOR_SIZE] = {'\0'};
	char imap_buf[HDD_SECTORS_PER_BLK * HDD_SECTOR_SIZE] ;
	int last_blocks = 0;

	/* ���� inode �ı�־λ */
	memset(&imap_buf, 0377, sizeof(imap_buf));
	for (i_idx = 0; i_idx < (8<<10)/8; ++i_idx)
		imap_buf[i_idx] = 0x0;
	
	/* ����ÿһ������ */
	for (g_idx = 0; g_idx < hdd_vars.total_groups; ++g_idx) {
		if (lseek64(hdd_vars.fd, offset + g_idx*g_size, SEEK_SET) < 0){
			printf("\nError: Failed to seek to bmap block!\n");
			return -1;
		}

		/* ���� bmap */
		if (0 == g_idx)	 /* ��λǰ2�����ݿ�, ���ڸ�Ŀ¼�ļ� */
			buf[0] = 0x03;
		if (write(hdd_vars.fd, &buf, sizeof(buf)) < 0) {
			printf("\nError: Failed to init bmap!\n");
			return -1;
		}
		if (0 == g_idx)	
			buf[0] = '\0';

		/* ���� imap */
		if (0 == g_idx)	{ /* ��λ 0~10 inode �ű�ռ�� */
			imap_buf[0] = 0xFF;
			imap_buf[1] = 0x07;
		}
		if (write(hdd_vars.fd, &imap_buf, sizeof(imap_buf)) < 0) {
			printf("\nError: Failed to init imap!\n");
			return -1;
		}
		if (0 == g_idx)	{
			imap_buf[0] = 0x0;
			imap_buf[1] = 0x0;
		}

		/* ����ÿһ�� inode table ���ݿ� */
		for (i_idx = 0; i_idx < hdd_vars.itable_blocks; ++i_idx) 
			if (write(hdd_vars.fd, &buf, sizeof(buf)) < 0) {
				printf("\nError: Failed to init inode table!\n");
				return -1;
			}
	}

	/* �������һ������� bmap */
	if (hdd_vars.blks_last_group < hdd_vars.blks_per_group) {
		last_blocks = hdd_vars.blks_last_group - 3
			- hdd_vars.gdt_blocks - hdd_vars.itable_blocks;

		/* ��λ�����ڵ����ݿ� bits */
		for (b_idx = last_blocks/8; b_idx < hdd_vars.block_size; ++b_idx)
			buf[b_idx] = 0xFF;

		offset = hdd_vars.block_size * (hdd_vars.itable_blocks + 2);
		if (lseek64(hdd_vars.fd, -offset, SEEK_CUR) < 0) {
			printf("\nError: Failed to seek to bmap block!\n");
			return -1;
		}

		if (write(hdd_vars.fd, &buf, sizeof(buf)) < 0) {
			printf("\nError: Failed to adjust bmap!\n");
			return -1;
		}
	}

	printf("\nInfo: Initialized b_i_map inode table!\n");
	return 0;
}

/* ��ʼ�� gdt ���������� */
static int hdd_init_gdts(void)
{
	int err = 0;
	u_int32_t g_idx = 0;
	u_int64_t offset = 2 * hdd_vars.block_size;
	size_t gdt_size = sizeof(struct hdd_group_desc) * hdd_vars.total_groups;
	struct hdd_group_desc * gdt = 
		(struct hdd_group_desc *) calloc(1, gdt_size);

	/* ����ȫ�� gdesc */
	for (g_idx = 0; g_idx < hdd_vars.total_groups; ++g_idx)	{
		gdt[g_idx].bg_block_bitmap = cpu_to_le32(2 +hdd_vars.gdt_blocks
			+ g_idx * hdd_vars.blks_per_group);
		gdt[g_idx].bg_inode_bitmap = cpu_to_le32(3 +hdd_vars.gdt_blocks
			+ g_idx * hdd_vars.blks_per_group);
		gdt[g_idx].bg_inode_table =  cpu_to_le32(4 +hdd_vars.gdt_blocks
			+ g_idx * hdd_vars.blks_per_group);
		gdt[g_idx].bg_free_blocks_count = 
			cpu_to_le32(hdd_vars.block_size * 8);	/* ���п��� */
		gdt[g_idx].bg_free_inodes_count = 
			cpu_to_le32(hdd_vars.inodes_per_group);	/* ���� inode �� */
	}
	
	/* ����gdt[0]: ��Ŀ¼�ļ���2����, 0-10��ino, 2��Ŀ¼ */
	gdt[0].bg_free_blocks_count = cpu_to_le32(hdd_vars.block_size * 8 - 2);
	gdt[0].bg_free_inodes_count = cpu_to_le32((8<<10) - HDD_FIRST_INO);
	gdt[0].bg_used_dirs_count = cpu_to_le32(2);

	/* �������һ�� gdesc */
	g_idx--;
	gdt[g_idx].bg_free_blocks_count =cpu_to_le32(hdd_vars.blks_last_group);

	/* д��������� */
	for(g_idx = 0; g_idx < hdd_vars.total_groups; ++ g_idx) {
		;
		if (lseek64(hdd_vars.fd, offset + g_idx * hdd_vars.group_size,
			SEEK_SET) < 0) {
			printf("\nError: Failed to seek to gdt!\n");
			err = -1;
			goto exit;
		}

		if (write(hdd_vars.fd, gdt, gdt_size) < 0) {
			printf("\nError: Failed to init gdt!\n");
			err = -1;
			goto exit;
		}
	}
exit:
	free(gdt);

	if (0 == err)
		printf("\nInfo: Initialized gdt.\n");
	return err;
}

/* ��ʼ�� root inode */
static int hdd_write_root_inode(void)
{
	struct hdd_inode rawi;
	u_int64_t offset = (4 + hdd_vars.gdt_blocks) * hdd_vars.block_size
		+ HDD_ROOT_INO * sizeof(struct hdd_inode);

	memset(&rawi, '\0', sizeof(rawi));
	rawi.i_mode = cpu_to_le16(0x41ed);		/* �ļ����ͺͷ���Ȩ�� */
	rawi.i_flags= cpu_to_le32(HDD_IF_ONHDD);	/* �ļ�Ǩ����Ϣ */
	rawi.i_links_count = cpu_to_le16(2);		/* Ӳ������ */
	rawi.i_uid  = cpu_to_le32(getuid());		/* Owner Uid */
	rawi.i_gid  = cpu_to_le32(getgid());		/* Group Id */
	rawi.i_size = cpu_to_le64(2 * hdd_vars.block_size);/* �ļ����� */
	rawi.i_blocks = cpu_to_le32(2);			/* ���� */
	rawi.i_atime = rawi.i_ctime = rawi.i_mtime
		     = cpu_to_le32(time(NULL));		/* ʱ�� */
	rawi.u.s_hdd.i_block[0] = cpu_to_le32(0);
	rawi.u.s_hdd.i_block[1] = cpu_to_le32(1);

	if (lseek64(hdd_vars.fd, offset, SEEK_SET) < 0)	{
		printf("\nError: Failed to seek to root inode!\n");
		return -1;
	}
	
	/* д inode ���豸 */
	if (write(hdd_vars.fd, &rawi, sizeof(rawi)) < 0) {
		printf("\nError: Failed to write the root inode!\n");
		return -1;
	}
	
	return 0;
}

/* ���Ĭ�ϸ�Ŀ¼�� */
static int hdd_add_root_dentry(void)
{
	struct hdd_dir_entry rootdir;
	char zeros[4096] = {0};
	u_int64_t offset = (4 + hdd_vars.gdt_blocks + hdd_vars.itable_blocks)
		* hdd_vars.block_size;

	if (lseek64(hdd_vars.fd, offset, SEEK_SET) < 0)	{
		printf("\nError: Failed to seek to root dentry block!\n");
		return -1;
	}

	/* �������Ŀ¼�ļ� */
	if (write(hdd_vars.fd, zeros, sizeof(zeros)) < 0
	||  write(hdd_vars.fd, zeros, sizeof(zeros)) < 0) {
		printf("\nError: Failed to write root dentry!\n");
		return -1;
	}

	memset(&rootdir, '\0', sizeof(rootdir));
	rootdir.ino = cpu_to_le32(HDD_ROOT_INO);
	rootdir.rec_len = cpu_to_le16(12);
	rootdir.name_len = 1;
	rootdir.file_type = HDD_FT_DIR;
	rootdir.name[0] = '.'; 

	if (lseek64(hdd_vars.fd, offset, SEEK_SET) < 0)	{
		printf("\nError: Failed to seek to root dentry block!\n");
		return -1;
	}

	/* д��Ŀ¼ */
	if (write(hdd_vars.fd, &rootdir, 12) < 0) {
		printf("\nError: Failed to write root dentry!\n");
		return -1;
	}

	rootdir.name_len = 2;
	rootdir.rec_len = cpu_to_le16(4084);
	rootdir.name[1] = '.';

	/* д��Ŀ¼ */
	if (write(hdd_vars.fd, &rootdir, 12) < 0) {
		printf("\nError: Failed to write root dentry!\n");
		return -1;
	}

	memset(&rootdir, '\0', sizeof(rootdir));
	rootdir.rec_len = cpu_to_le16(4096); /* ռ�ø�Ŀ¼�ĵڶ������ݿ� */

	if (lseek64(hdd_vars.fd, offset+4096, SEEK_SET) < 0)	{
		printf("\nError: Failed to seek to root dentry block!\n");
		return -1;
	}

	/* ռ�ø�Ŀ¼�ĵڶ������ݿ� */
	if (write(hdd_vars.fd, &rootdir, 12) < 0) {
		printf("\nError: Failed to write root dentry!\n");
		return -1;
	}

	printf("\nInfo: Success to write the root dentries!\n");
	return 0;
}

/* д������ */
static int hdd_write_super_block(void)
{
	u_int32_t s_idx = 0;
	u_int32_t sg[3] = {0}; /* ��� 3 �������鱸�� */
	u_int64_t offset = 0;
	
	sg[1] = hdd_vars.total_groups / 2;
	sg[2] = hdd_vars.total_groups - 1;
	if (sg[1] == sg[2])
		sg[1] = 0;
	
	/* д������ */
	for (s_idx = 0; s_idx < 3; ++s_idx) {
		if (s_idx > 0 && sg[s_idx] == 0)
			continue;
			
		hdd_sb.s_block_group_nr = cpu_to_le32(sg[s_idx]);
		offset = hdd_vars.block_size + sg[s_idx] * hdd_vars.group_size;
		
		if (lseek64(hdd_vars.fd, offset, SEEK_SET) < 0){
			printf("\nError: Failed to seek to super block!\n");
			return -1;
		}

		if (write(hdd_vars.fd, &hdd_sb, sizeof(hdd_sb)) < 0) {
			printf("\nError: Failed to write the super block!\n");
			return -1;
		}		
	}

	printf("\nInfo: Created super blocks.\n");
	return 0;
}

/* ��ʽ���豸 */
static int hdd_format_device(void)
{
	int err = 0;

	err= hdd_prepare_sb();		/* ��ʼ��������ṹ */
	if (err < 0)
		goto exit;
	
	err = hdd_trim_device();	/* �޼��豸 */
	if (err < 0) {
		printf("\n\tError: Failed to trim hdd device!\n");
		goto exit;
	}

	err = hdd_init_bmap_imap_itable();/* ��ʼ��λͼ�� inode table */
	if (err < 0) {
		printf("\n\tError: Failed to init b_i_map inode table!\n");
		goto exit;
	}

	err = hdd_init_gdts();		/* ��ʼ�� gdt ���������� */
	if (err < 0) {
		printf("\n\tError: Failed to init GDT!\n");
		goto exit;
	}

	err = hdd_write_root_inode();	/* ��ʼ�� root inode */
	if (err < 0) 
		goto exit;

	err = hdd_add_root_dentry();	/* ���Ĭ�ϸ�Ŀ¼�� */
	if (err < 0)
		goto exit;

	printf("\nInfo: Created root dir . and ..\n");
	
	err = hdd_write_super_block();	/* д������ */
	if (err < 0) {
		printf("\n\tError: Failed to write the Super Block!\n");
		goto exit;
	}
exit:
	if (err)
		printf("\n\tError: Could not format the hdd device!\n");

	if (fsync(hdd_vars.fd) < 0)
		printf("\n\tError: Could not conduct fsync!\n");

	if (close(hdd_vars.fd) < 0)
		printf("\n\tError: Failed to close hdd device file!\n");

	return err;
}

/* ������ */
int main(int argc, char *argv[])
{
	printf("\nfmc_hdd: formatting hdd volume...\n");
	
	hdd_init_vars();		/* ��ʼ��������� */
	
	hdd_parse_options(argc, argv);	/* ����������ѡ�� */

	if(device_is_mounted(hdd_vars.hdd_device) < 0)/* �����Ƿ��ѱ����� */
		return -1;

	if(hdd_get_device_info() < 0)	/* ��ȡ������Ϣ */
		return -1;
	
	if(get_and_set_uuid() < 0)	/* ��ȡĿ�� ssd �� UUID */
		return -1;

	if(hdd_format_device() < 0)	/* ��ʽ���豸 */
		return -1;

	printf("\nInfo: fmc_hdd success. \n\n");

	return 0;
}

