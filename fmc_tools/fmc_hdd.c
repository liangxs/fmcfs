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
 *	-f: 优先迁移的文件类型, 待实现
 *	-w: 需要整个迁移到 SSD 的文件类型, 如图像, 待实现
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

/* 输出 mkfs.fmc_hdd 命令用法 */
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

/* 初始化全局参数 */
static void hdd_init_vars(){

	memset(&hdd_vars, '\0', sizeof(hdd_vars));

	hdd_vars.block_size = 1 << hdd_vars.log_block_size << 12;

	hdd_vars.ratio = DEF_HDD_MAXRATIO;
	hdd_vars.age = DEF_HDD_MAXAGE;

	uuid_generate(hdd_vars.hdd_uuid);

	hdd_vars.sector_size = HDD_SECTOR_SIZE;		/* 设备扇区大小 */

	hdd_vars.inodes_per_group = 8 << 10; /* 固定为 8K 个 inode */
	hdd_vars.itable_blocks = HDD_INODE_SIZE * hdd_vars.inodes_per_group
		/ hdd_vars.block_size;
}

/* 解析命令选项 */
static void hdd_parse_options(int argc, char *argv[])
{
	static const char *option_str = "a:b:c:s:r:";
	int32_t option = 0;
	int32_t tmp = 0;

	while ((option = getopt(argc, argv, option_str)) != EOF)
		switch (option) {
		case 'a':/* 最大未访问时间 */
			hdd_vars.age = atoi(optarg);
			if (hdd_vars.age < 3600)
				hdd_vars.age = 3600;/* 最小寿命 1 小时 */
			break;
		case 'b':/* 块长 */
			tmp = atoi(optarg);
			if (tmp != 4)
				hdd_usage();
			break;
		case 'c':/* 云服务名称 */
			if (strlen(optarg) >= CLD_NAME_LEN) {
				printf("Error: Cloud name is too long!\n");
				hdd_usage();
			}			
			hdd_vars.cld_service = strdup(optarg);
			break;
		case 's':/* ssd设备名称 */
			hdd_vars.ssd_device = strdup(optarg);
			break;
		case 'r':/* 最大空间使用率 */
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

/* 检查设备是否被挂载 */
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

/* 读取设备信息 */
static int hdd_get_device_info(void)
{
	int32_t fd = 0;
	int32_t sector_size = 0;
	struct stat stat_buf;
	struct hd_geometry geom;

	/* 打开设备文件 */
	fd = open(hdd_vars.hdd_device, O_RDWR);
	if (fd < 0) {
		printf("\n\tError: Failed to open the HDD device: %s!\n",
			hdd_vars.hdd_device);
		return -1;
	}
	hdd_vars.fd = fd;

	/* 读取文件状态 */
	if (fstat(fd, &stat_buf) < 0 ) {
		printf("\n\tError: Failed to get the HDD device stat: %s!\n",
			hdd_vars.hdd_device);
		return -1;
	}

	/* 计算扇区总数 */
	if (S_ISREG(stat_buf.st_mode)) {	/* 常规文件 */
		hdd_vars.total_sectors = stat_buf.st_size / 512;
	}else if (S_ISBLK(stat_buf.st_mode)) {	/* 块设备文件 */
		if (ioctl(fd, BLKSSZGET, &sector_size) < 0 ) /* 获取扇区大小 */
			printf("\n\tError: Cannot get the HDD sector size! \
			       Using the default Sector Size\n");

		/* 读取扇区总数 */
		if (ioctl(fd, BLKGETSIZE, &hdd_vars.total_sectors) < 0) {
			printf("\n\tError: Cannot get the HDD device size\n");
			return -1;
		}

		/* 读取设备参数 */
		if (ioctl(fd, HDIO_GETGEO, &geom) < 0) {
			printf("\n\tError: Cannot get the HDD device geometry\n");
			return -1;
		}
		hdd_vars.start_sector = geom.start; /* 起始扇区 */
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

	/* 判断设备容量是否够大 */
	if (hdd_vars.total_sectors <(FMC_MIN_VOLUME_SIZE / HDD_SECTOR_SIZE)) {
		printf("Error: Min volume size supported is %d Bytes\n",
			FMC_MIN_VOLUME_SIZE);
		return -1;
	}

	return 0;
}

/* 读取 ssd 的 UUID, 并设置 hdd 的 UUID */
static int get_and_set_uuid(void)
{
	int fd = -1;
	int idx = 0;
	__u8 zerouuid[16] = {'\0'};
	struct stat stat_buf;

	if(hdd_vars.ssd_device == NULL)	/* 不使用 SSD */
		return 0;

	fd = open(hdd_vars.ssd_device, O_RDWR);/* 打开 ssd 设备文件 */
	if (fd < 0) {
		printf("\n\tError: Cannot open SSD device: %s\n", 
			hdd_vars.ssd_device);
		return -1;
	}

	if (fstat(fd, &stat_buf) < 0) {	/* 读取设备状态 */
		printf("\n\tError: Failed to get the SSD stat: %s!\n",
			hdd_vars.ssd_device);
		return -1;
	}
	
	/* 判断设备类型: 常规文件或块设备 */
	if(S_ISREG(stat_buf.st_mode) || S_ISBLK(stat_buf.st_mode)) {

		/* 跳过首个保留的数据块 */
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

		/* 判断是否为 fmc_ssd 类型设备 */
		if (le32_to_cpu(ssd_sb.s_magic) != SSD_MAGIC) {
			printf("\n\tError: %s is not a fmc_ssd volume!\n",
				hdd_vars.ssd_device);
			return -1;
		}
		/* 判断 SSD 和 HDD 的块大小是否相同 */
		if (1 << le32_to_cpu(ssd_sb.s_log_blocksize)
		!= hdd_vars.block_size) {
			printf("\n\tError: %s block size neq hdd block size!\n",
				hdd_vars.ssd_device);
			return -1;
		}
		/* 判断 SSD 中是否还有空闲 HDD 位置 */
		if (ssd_sb.s_hdd_count >= SSD_MAX_HDDS) {
			printf("\nError: ssd %s already has %d hdd volumes!\n",
				hdd_vars.ssd_device, SSD_MAX_HDDS);
			return -1;
		}

		/* 记录目标 SSD 的 UUID */
		memcpy(hdd_vars.ssd_uuid, ssd_sb.s_uuid, 
			sizeof(hdd_vars.ssd_uuid));

		/* 若 ssd 未挂载, 则添加此 hdd 的 uuid 信息 */
		if (!device_is_mounted(hdd_vars.ssd_device)) {
			ssd_sb.s_hdd_count = 
				le32_to_cpu(cpu_to_le32(ssd_sb.s_hdd_count)+1);

			/* 查找空位 */
			for (idx = 0; idx < SSD_MAX_HDDS; ++idx)
				if (memcmp(ssd_sb.hdd_uuids[idx], zerouuid, 
					sizeof(zerouuid)) == 0)
				break;
			
			if (idx < SSD_MAX_HDDS) {
				memcpy(ssd_sb.hdd_uuids[idx], hdd_vars.hdd_uuid,
					sizeof(hdd_vars.hdd_uuid));
				hdd_sb.s_hdd_idx = cpu_to_le32(idx);
			}

			/* 写入 SSD 中 */
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
	u_int32_t gdt_blocks = 0;	/* 组描述表的大小 */
	u_int32_t blocks_per_group = 0;
	u_int32_t blks_of_last_group;

	/* 先计算最大的可能块组个数 */
	max_groups = hdd_vars.total_blocks
		   / (1+1+1+1+ 8*1024*HDD_INODE_SIZE/hdd_vars.block_size
		       + hdd_vars.block_size * 8);
	max_groups ++;

	/* 尝试 groups 的最佳个数 */
	if (max_groups < 5)
		max_groups = 5;
	for(groups = max_groups - 4; groups <= max_groups; ++groups) {
		/* gdt 的上取整块数 */
		gdt_blocks = (groups * 32 + hdd_vars.block_size - 1)
			   / hdd_vars.block_size;
		/* 每块组的块数 */
		blocks_per_group = 1+gdt_blocks+1+1+hdd_vars.itable_blocks
				 + (hdd_vars.block_size * 8);
		/* 由此块组数得到的总块数刚超过实际总块数 */
		if (groups * blocks_per_group >= hdd_vars.total_blocks - 1)
			break;
	}

	/* 最后一个块组的实际块数 */
	blks_of_last_group = hdd_vars.total_blocks - 1 /* boot 块 */
			   - (groups-1) * blocks_per_group;
	hdd_vars.blks_last_group = blks_of_last_group;
	/* 最后一个块组若的数据部分少于256块(1MB), 则不计之 */
	if (blks_of_last_group < 3+gdt_blocks+hdd_vars.itable_blocks + 256){
		groups--;
		gdt_blocks = (groups * 32 + hdd_vars.block_size - 1)
			   / hdd_vars.block_size;

		blocks_per_group = 3 + gdt_blocks + hdd_vars.itable_blocks
			         + (hdd_vars.block_size * 8);

		/* 调整总块数 */
		hdd_vars.total_blocks = groups * blocks_per_group + 1;
		hdd_vars.blks_last_group = blocks_per_group;
	} else {/* 使最后块组的可用数据块的个数为 8 的整数倍 */
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

/* 初始化超级块结构 */
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
	hdd_sb.s_inodes_per_group = cpu_to_le32(8 << 10); /* 固定 8K 个 inodes */
	hdd_sb.s_first_ino	= cpu_to_le32(HDD_FIRST_INO);

	calc_groups();
	hdd_sb.s_groups_count	= cpu_to_le32(hdd_vars.total_groups);
	hdd_sb.s_blocks_count	= cpu_to_le32(hdd_vars.total_blocks);
	hdd_sb.s_inodes_count	= cpu_to_le32((8 << 10)*hdd_vars.total_groups);
	hdd_sb.s_gdt_blocks	= cpu_to_le32(hdd_vars.gdt_blocks);
	hdd_sb.s_itable_blocks	= cpu_to_le32(hdd_vars.itable_blocks);

	hdd_sb.s_user_blocks = cpu_to_le32(2);/* 根目录文件的数据块数 */
	hdd_sb.s_free_blocks_count=cpu_to_le32(hdd_vars.total_blocks - 1/*boot*/
		- hdd_vars.total_groups * (3 + hdd_vars.gdt_blocks
		+ hdd_vars.itable_blocks) - 2/* 根目录文件的2块 */);

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
		strncpy(hdd_sb.s_ssd_name, hdd_vars.ssd_device,/* ssd 设备名 */
		sizeof(hdd_sb.s_ssd_name));
	hdd_sb.s_hdd_idx = cpu_to_le32(-1);

	if (NULL != hdd_vars.cld_service)
		strncpy(hdd_sb.s_cld_name, hdd_vars.cld_service,/* cloud 名 */
		sizeof(hdd_sb.s_cld_name));

	hdd_sb.s_upper_ratio	= cpu_to_le32(hdd_vars.ratio);	/* 最大空间使用率 */
	hdd_sb.s_max_unaccess	= cpu_to_le32(hdd_vars.age);	/* 文件访问年龄 - 秒 */

	return 0;
}

/* 修剪设备 */
static int hdd_trim_device(void)
{
	unsigned long long range[2];
	struct stat stat_buf;

	range[0] = 0;
	range[1] = hdd_vars.total_blocks * HDD_SECTORS_PER_BLK * HDD_SECTOR_SIZE;

	/* 读取设备文件状态 */
	if (fstat(hdd_vars.fd, &stat_buf) < 0 ) {
		printf("\n\tError: Failed to get the device stat!\n");
		return -1;
	}

	if (S_ISREG(stat_buf.st_mode))
		return 0;
	else if (S_ISBLK(stat_buf.st_mode)) {
		/* 设置设备的丢弃部分 */
		if (ioctl(hdd_vars.fd, BLKDISCARD, &range) < 0)
			printf("Info: This device doesn't support TRIM\n");
	} else
		return -1;

	return 0;
}

/*  初始化位图和 inode table */
static int hdd_init_bmap_imap_itable(void)
{
	u_int32_t g_idx = 0, i_idx = 0, b_idx = 0;
	u_int64_t g_size = hdd_vars.group_size;
	u_int64_t offset = hdd_vars.block_size * (2 + hdd_vars.gdt_blocks);
	char buf[HDD_SECTORS_PER_BLK * HDD_SECTOR_SIZE] = {'\0'};
	char imap_buf[HDD_SECTORS_PER_BLK * HDD_SECTOR_SIZE] ;
	int last_blocks = 0;

	/* 保留 inode 的标志位 */
	memset(&imap_buf, 0377, sizeof(imap_buf));
	for (i_idx = 0; i_idx < (8<<10)/8; ++i_idx)
		imap_buf[i_idx] = 0x0;
	
	/* 处理每一个块组 */
	for (g_idx = 0; g_idx < hdd_vars.total_groups; ++g_idx) {
		if (lseek64(hdd_vars.fd, offset + g_idx*g_size, SEEK_SET) < 0){
			printf("\nError: Failed to seek to bmap block!\n");
			return -1;
		}

		/* 处理 bmap */
		if (0 == g_idx)	 /* 置位前2个数据块, 用于根目录文件 */
			buf[0] = 0x03;
		if (write(hdd_vars.fd, &buf, sizeof(buf)) < 0) {
			printf("\nError: Failed to init bmap!\n");
			return -1;
		}
		if (0 == g_idx)	
			buf[0] = '\0';

		/* 处理 imap */
		if (0 == g_idx)	{ /* 置位 0~10 inode 号被占用 */
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

		/* 处理每一个 inode table 数据块 */
		for (i_idx = 0; i_idx < hdd_vars.itable_blocks; ++i_idx) 
			if (write(hdd_vars.fd, &buf, sizeof(buf)) < 0) {
				printf("\nError: Failed to init inode table!\n");
				return -1;
			}
	}

	/* 调整最后一个块组的 bmap */
	if (hdd_vars.blks_last_group < hdd_vars.blks_per_group) {
		last_blocks = hdd_vars.blks_last_group - 3
			- hdd_vars.gdt_blocks - hdd_vars.itable_blocks;

		/* 置位不存在的数据块 bits */
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

/* 初始化 gdt 组描述符表 */
static int hdd_init_gdts(void)
{
	int err = 0;
	u_int32_t g_idx = 0;
	u_int64_t offset = 2 * hdd_vars.block_size;
	size_t gdt_size = sizeof(struct hdd_group_desc) * hdd_vars.total_groups;
	struct hdd_group_desc * gdt = 
		(struct hdd_group_desc *) calloc(1, gdt_size);

	/* 设置全部 gdesc */
	for (g_idx = 0; g_idx < hdd_vars.total_groups; ++g_idx)	{
		gdt[g_idx].bg_block_bitmap = cpu_to_le32(2 +hdd_vars.gdt_blocks
			+ g_idx * hdd_vars.blks_per_group);
		gdt[g_idx].bg_inode_bitmap = cpu_to_le32(3 +hdd_vars.gdt_blocks
			+ g_idx * hdd_vars.blks_per_group);
		gdt[g_idx].bg_inode_table =  cpu_to_le32(4 +hdd_vars.gdt_blocks
			+ g_idx * hdd_vars.blks_per_group);
		gdt[g_idx].bg_free_blocks_count = 
			cpu_to_le32(hdd_vars.block_size * 8);	/* 空闲块数 */
		gdt[g_idx].bg_free_inodes_count = 
			cpu_to_le32(hdd_vars.inodes_per_group);	/* 空闲 inode 数 */
	}
	
	/* 调整gdt[0]: 根目录文件的2个块, 0-10号ino, 2个目录 */
	gdt[0].bg_free_blocks_count = cpu_to_le32(hdd_vars.block_size * 8 - 2);
	gdt[0].bg_free_inodes_count = cpu_to_le32((8<<10) - HDD_FIRST_INO);
	gdt[0].bg_used_dirs_count = cpu_to_le32(2);

	/* 调整最后一个 gdesc */
	g_idx--;
	gdt[g_idx].bg_free_blocks_count =cpu_to_le32(hdd_vars.blks_last_group);

	/* 写入各个块组 */
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

/* 初始化 root inode */
static int hdd_write_root_inode(void)
{
	struct hdd_inode rawi;
	u_int64_t offset = (4 + hdd_vars.gdt_blocks) * hdd_vars.block_size
		+ HDD_ROOT_INO * sizeof(struct hdd_inode);

	memset(&rawi, '\0', sizeof(rawi));
	rawi.i_mode = cpu_to_le16(0x41ed);		/* 文件类型和访问权限 */
	rawi.i_flags= cpu_to_le32(HDD_IF_ONHDD);	/* 文件迁移信息 */
	rawi.i_links_count = cpu_to_le16(2);		/* 硬链接数 */
	rawi.i_uid  = cpu_to_le32(getuid());		/* Owner Uid */
	rawi.i_gid  = cpu_to_le32(getgid());		/* Group Id */
	rawi.i_size = cpu_to_le64(2 * hdd_vars.block_size);/* 文件长度 */
	rawi.i_blocks = cpu_to_le32(2);			/* 块数 */
	rawi.i_atime = rawi.i_ctime = rawi.i_mtime
		     = cpu_to_le32(time(NULL));		/* 时间 */
	rawi.u.s_hdd.i_block[0] = cpu_to_le32(0);
	rawi.u.s_hdd.i_block[1] = cpu_to_le32(1);

	if (lseek64(hdd_vars.fd, offset, SEEK_SET) < 0)	{
		printf("\nError: Failed to seek to root inode!\n");
		return -1;
	}
	
	/* 写 inode 到设备 */
	if (write(hdd_vars.fd, &rawi, sizeof(rawi)) < 0) {
		printf("\nError: Failed to write the root inode!\n");
		return -1;
	}
	
	return 0;
}

/* 添加默认根目录项 */
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

	/* 首先清空目录文件 */
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

	/* 写根目录 */
	if (write(hdd_vars.fd, &rootdir, 12) < 0) {
		printf("\nError: Failed to write root dentry!\n");
		return -1;
	}

	rootdir.name_len = 2;
	rootdir.rec_len = cpu_to_le16(4084);
	rootdir.name[1] = '.';

	/* 写根目录 */
	if (write(hdd_vars.fd, &rootdir, 12) < 0) {
		printf("\nError: Failed to write root dentry!\n");
		return -1;
	}

	memset(&rootdir, '\0', sizeof(rootdir));
	rootdir.rec_len = cpu_to_le16(4096); /* 占用根目录的第二个数据块 */

	if (lseek64(hdd_vars.fd, offset+4096, SEEK_SET) < 0)	{
		printf("\nError: Failed to seek to root dentry block!\n");
		return -1;
	}

	/* 占用根目录的第二个数据块 */
	if (write(hdd_vars.fd, &rootdir, 12) < 0) {
		printf("\nError: Failed to write root dentry!\n");
		return -1;
	}

	printf("\nInfo: Success to write the root dentries!\n");
	return 0;
}

/* 写超级块 */
static int hdd_write_super_block(void)
{
	u_int32_t s_idx = 0;
	u_int32_t sg[3] = {0}; /* 最多 3 个超级块备份 */
	u_int64_t offset = 0;
	
	sg[1] = hdd_vars.total_groups / 2;
	sg[2] = hdd_vars.total_groups - 1;
	if (sg[1] == sg[2])
		sg[1] = 0;
	
	/* 写超级块 */
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

/* 格式化设备 */
static int hdd_format_device(void)
{
	int err = 0;

	err= hdd_prepare_sb();		/* 初始化超级块结构 */
	if (err < 0)
		goto exit;
	
	err = hdd_trim_device();	/* 修剪设备 */
	if (err < 0) {
		printf("\n\tError: Failed to trim hdd device!\n");
		goto exit;
	}

	err = hdd_init_bmap_imap_itable();/* 初始化位图和 inode table */
	if (err < 0) {
		printf("\n\tError: Failed to init b_i_map inode table!\n");
		goto exit;
	}

	err = hdd_init_gdts();		/* 初始化 gdt 组描述符表 */
	if (err < 0) {
		printf("\n\tError: Failed to init GDT!\n");
		goto exit;
	}

	err = hdd_write_root_inode();	/* 初始化 root inode */
	if (err < 0) 
		goto exit;

	err = hdd_add_root_dentry();	/* 添加默认根目录项 */
	if (err < 0)
		goto exit;

	printf("\nInfo: Created root dir . and ..\n");
	
	err = hdd_write_super_block();	/* 写超级块 */
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

/* 主函数 */
int main(int argc, char *argv[])
{
	printf("\nfmc_hdd: formatting hdd volume...\n");
	
	hdd_init_vars();		/* 初始化各项参数 */
	
	hdd_parse_options(argc, argv);	/* 解析命令行选项 */

	if(device_is_mounted(hdd_vars.hdd_device) < 0)/* 磁盘是否已被挂载 */
		return -1;

	if(hdd_get_device_info() < 0)	/* 读取磁盘信息 */
		return -1;
	
	if(get_and_set_uuid() < 0)	/* 读取目标 ssd 的 UUID */
		return -1;

	if(hdd_format_device() < 0)	/* 格式化设备 */
		return -1;

	printf("\nInfo: fmc_hdd success. \n\n");

	return 0;
}

