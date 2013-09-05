/*
 * fmc_ssd.c 
 */

/* Usage: mkfs -t fmc_ssd [options] device 
 *
 * eg: mkfs -t fmc_ssd [-b 4] [-r 80] /dev/sdb1
 *	-b: blocksize, can be 4 KB.
 *	-r: upper limitation ratio, used for trigging migration
 *	device: ssd device name
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <mntent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <linux/hdreg.h>
#include <time.h>
#include <linux/fs.h>
#include <uuid/uuid.h>

#include "fmc_ssd.h"

struct ssd_global_vars	ssd_vars;	/* ȫ�ֲ��� */
struct ssd_sb_const	sbc;		/* �����鲻�ױ䲿�� */
struct ssd_sb_volatile	sbv;		/* �������ױ䲿�� */

/* ��ʼ��ȫ�ֲ��� */
static void ssd_init_global_vars(void)
{
	memset(&ssd_vars, '\0', sizeof(ssd_vars));

	ssd_vars.sector_size	= SSD_SECTOR_SIZE;
	ssd_vars.sectors_per_blk= SSD_SECTORS_PER_BLK;
	ssd_vars.blk_kilo_size	= 4;
	ssd_vars.blks_per_seg	= SSD_BLKS_PER_SEG;
	ssd_vars.segs_per_sec	= SSD_SEGS_PER_SEC;
	ssd_vars.upper_ratio	= SSD_DEF_MAXRATIO;
}

/* ��� fmc_ssd �����÷� */
static void ssd_usage(void)
{
	fprintf(stderr, 
	"\nUsage: mkfs -t fmc_ssd [options] device\n"
	"\teg: mkfs -t fmc_ssd [-b 4] [-r 80] /dev/sdb1\n"
	"\t-b: blocksize, can be 4 KB.[default:4]\n"
	"\t-r: [20-95], upper used ratio.[default:80]\n"
	"\tdevice: ssd device name\n\n");

	exit(1);
}

/* ���������в��� */
static void ssd_parse_options(int argc, char *argv[])
{
	static const char *opt_string = "b:r:";
	int32_t option = 0;
	int32_t tmp = 0;

	while ((option = getopt(argc,argv,opt_string)) != EOF) 
		switch(option) {
		case 'b':
			tmp = atoi(optarg);
			if (tmp!=4)
				ssd_usage();
			ssd_vars.blk_kilo_size = tmp;
			ssd_vars.sectors_per_blk = (tmp << 10)/ssd_vars.sector_size;

			printf("Info: block size is %d KB.\n", tmp);
			break;
		case 'r':
			tmp = atoi(optarg);
			if (tmp < 20 || tmp > 95)
				ssd_usage();
			ssd_vars.upper_ratio = tmp;

			printf("Info: upper_ratio is %d%%.\n", tmp);
			break;
		default:
			printf("Error: Unknown option: %c.\n", option);
			ssd_usage();
			break;
		}

	if ((optind + 1) != argc) {	/* ����Ƿ��ṩ���豸�ļ� */
		printf("Error: Device not specified.\n");
		ssd_usage();
	}
	ssd_vars.device_name = argv[optind];
}

/* ssd �豸�Ƿ��ѹ��� */
static int ssd_is_mounted(void)
{
	FILE *file;
	struct mntent *mnt; /* ���ڻ�ȡ mount ��Ϣ */

	if ((file = setmntent(MOUNTED, "r")) == NULL)
		return 0;

	/* ���μ��ÿ���������豸 */
	while ((mnt = getmntent(file)) != NULL) {
		if (!strcmp(ssd_vars.device_name, mnt->mnt_fsname)) {
			printf("Error: %s is already mounted\n",
				ssd_vars.device_name);
			return -1;
		}
	}

	endmntent(file);
	return 0;
}

/* ȡ���豸��Ϣ */
static int ssd_get_device_info(void)
{
	int32_t fd = 0;
	int32_t sector_size;
	struct stat stat_buf;
	struct hd_geometry geom; /* �豸��Ϣ */

	/* ���豸�ļ� */
	fd = open(ssd_vars.device_name, O_RDWR);
	if (fd < 0) {
		printf("\n\tError: Failed to open the device!\n");
		return -1;
	}
	ssd_vars.fd = fd;

	/* ��ȡ�ļ�״̬ */
	if (fstat(fd, &stat_buf) < 0 ) {
		printf("\n\tError: Failed to get the device stat!!!\n");
		return -1;
	}

	/* ���������������� */
	if (S_ISREG(stat_buf.st_mode)) {	/* �����ļ� */
		ssd_vars.total_sectors = stat_buf.st_size /
			ssd_vars.sector_size;
	} else if (S_ISBLK(stat_buf.st_mode)) {	/* ���豸�ļ� */
		if (ioctl(fd, BLKSSZGET, &sector_size) < 0 ) /* ��ȡ������С */
			printf("\n\tError: Cannot get the sector size!!! \
			       Using the default Sector Size\n");

		/* ��ȡ�������� */
		if (ioctl(fd, BLKGETSIZE, &ssd_vars.total_sectors) < 0) {
			printf("\n\tError: Cannot get the device size\n");
			return -1;
		}

		/* ��ȡ�豸���� - ��ʼ���� */
		if (ioctl(fd, HDIO_GETGEO, &geom) < 0) {
			printf("\n\tError: Cannot get the device geometry\n");
			return -1;
		}
		ssd_vars.start_sector = geom.start;
	} else {
		printf("\n\n\tError: SSD volume type is not supported!!!\n");
		return -1;
	}

	/* �ж��豸�����Ƿ񹻴� */
	if (ssd_vars.total_sectors <
		(FMC_MIN_VOLUME_SIZE / SSD_SECTOR_SIZE)) {
			printf("Error: Min volume size supported is %d\n",
				FMC_MIN_VOLUME_SIZE);
			return -1;
	}

	ssd_vars.total_blks = ssd_vars.total_sectors >> 3;/* �ܿ���, ��ȥ���� 1 ������� */

	ssd_vars.total_segs = (ssd_vars.total_blks - 1) >> 9;/* �ܶ���, ��ȥ���� 1 �εĿ� */

	ssd_vars.total_secs = (ssd_vars.total_segs - 1) >> 8;/* ������ */

	ssd_vars.last_sec = ssd_vars.total_segs - 1 - (ssd_vars.total_secs << 8);
	if(ssd_vars.last_sec >= 5)
		ssd_vars.total_secs ++;		/* �����һ������5��, �����֮ */
	else {
		ssd_vars.total_segs = (ssd_vars.total_secs << 8) + 1;/* ������ȥ */
		ssd_vars.last_sec = 256;
	}

	if (ssd_vars.total_secs > SSD_MAX_SECS) {/* ssd ���� */
		printf("\n\n\tError: SSD volume is too big!!!\n");
		return -1;
	}
	
	ssd_vars.total_blks = (ssd_vars.total_segs << 9) + 1;

	ssd_vars.total_sectors = ssd_vars.total_blks;
	ssd_vars.total_sectors <<= 3;

	printf("Info: total sectors = %"PRIu64" (in 512bytes)\n",
		ssd_vars.total_sectors);
	printf("Info: total blocks = %u (in 4KB)\n", ssd_vars.total_blks);
	printf("Info: total segments = %u (in 2MB)\n", ssd_vars.total_segs);
	printf("Info: total sects = %u (in 512MB)\n", ssd_vars.total_secs);

	return 0;
}

/* ��ʼ�� ssd ������ṹ */
static int ssd_prepare_sbs(void)
{
	u_int32_t i=0;

	memset(&sbc, '\0', sizeof(sbc));

	sbc.s_magic	= cpu_to_le32(SSD_MAGIC);
	sbc.s_major_ver	= cpu_to_le32(FMC_MAJOR_VERSION);
	sbc.s_minor_ver	= cpu_to_le32(FMC_MINOR_VERSION);
	sbc.s_log_sectorsize = cpu_to_le32(9);
	sbc.s_log_blocksize  = cpu_to_le32(12);
	sbc.s_log_seg_size   = cpu_to_le32(12 + 9); /* 2M */
	sbc.s_log_sec_size   = cpu_to_le32(12 + 9 + 8);/* 2M*256 */
	sbc.s_log_sectors_per_blk = cpu_to_le32(3);
	sbc.s_log_blocks_per_seg = cpu_to_le32(9);/* ���п���:9, �� log(512) */
	sbc.s_log_segs_per_sec= cpu_to_le32(8);	/* ���ж���:8, �� log(256) */
	sbc.s_sit_segs_per_sec= cpu_to_le32(1);	/* ���е� sit ����: 1 */

	uuid_generate(sbc.s_uuid);
	sbc.s_seg0_blkaddr = cpu_to_le32(1);	/* �׸��ε���ʼ���:1 */

	sbc.s_block_count= cpu_to_le32(ssd_vars.total_blks);/* �ܿ��� */
	sbc.s_seg_count	= cpu_to_le32(ssd_vars.total_segs);/* �ܶ���(���� boot ��) */
	sbc.s_sec_count = cpu_to_le32(ssd_vars.total_secs);/* ������(���� sb ��) */
	sbc.s_segs_last_sec = cpu_to_le32(ssd_vars.last_sec);/* ������Ķ��� */
	sbc.s_usr_blk_count = cpu_to_le32(ssd_vars.total_blks - 1 /* ��ʼ�� */
		- (ssd_vars.total_secs + 1) * 512); /* �û��Ŀ��� */

	memset(&sbv, '\0', sizeof(sbv));
	sbv.sb_verion = cpu_to_le32(1);		/* sb �汾�� */

	sbv.free_seg_count = cpu_to_le32(ssd_vars.total_segs - 1
		- ssd_vars.total_secs);		/* �ܵĿ��ж��� */

	for(i = 0; i < ssd_vars.total_secs -1 ; ++i) {
		sbv.free_segs[i] = 255;
		sbv.free_blks[i] = cpu_to_le32(512 * 255);/* �����п��п��� */
	}
	/* �������һ���� */
	sbv.free_segs[ssd_vars.total_secs - 1] = ssd_vars.last_sec - 1;
	sbv.free_blks[ssd_vars.total_secs - 1] = 
		cpu_to_le32(512 * (ssd_vars.last_sec - 1));
		
	return 0;
}

/* �޼� ssd �豸 */
static int ssd_trim_device(void)
{
	unsigned long long range[2];
	struct stat stat_buf;

	range[0] = 0;
	range[1] = ssd_vars.total_sectors * SSD_SECTOR_SIZE;

	/* ��ȡ�豸�ļ�״̬ */
	if (fstat(ssd_vars.fd, &stat_buf) < 0 ) {
		printf("\n\tError: Failed to get the device stat!!!\n");
		return -1;
	}

	if (S_ISREG(stat_buf.st_mode))
		return 0;
	else if (S_ISBLK(stat_buf.st_mode)) {
		/* �����豸�Ķ������� */
		if (ioctl(ssd_vars.fd, BLKDISCARD, &range) < 0)
			printf("Info: This device doesn't support TRIM\n");
	} else
		return -1;
	return 0;
}

/* ��ʼ��������Ϣ */
static int ssd_init_secs_info()
{
	int i = 0;
	u_int32_t sec_idx = 0;
	u_int32_t seg_idx = 0;
	u_int64_t offset = SSD_SECTORS_PER_BLK * SSD_SECTOR_SIZE
		+ (1 << 21);/* �׸�����ƫ���� */
	struct ssd_sit sit;
	struct seg_blocks_info segbi;
	struct sec_trans_table sec_table;
	struct sec_trans_map sec_map;

	memset(&sit, '\0', sizeof(sit));
	sit.stat = cpu_to_le16(SEG_FREE);	/* ��״̬ */
	sit.hdd_idx = cpu_to_le16(-1);	/* ���� hdd */
	sit.free_blocks = cpu_to_le32(SSD_BLKS_PER_SEG); /* ���п��� */

	memset(&segbi, '\0', sizeof(segbi));
	memset(&sec_table, '\0', sizeof(sec_table));

	memset(&sec_map, '\0', sizeof(sec_map));
	for (i = 0; i < SSD_BLKS_PER_SEG / 8; ++i)/* ������Ϣ�� */
		sec_map.map[i] = 0xFF;

	for (sec_idx = 0; sec_idx < ssd_vars.total_secs; ++sec_idx) {
		if (lseek64(ssd_vars.fd, offset, SEEK_SET) < 0) {
			printf("\n\tError: While lseek to the derised location!!!\n");
			return -1;
		}

		/* дÿ���εĿ���Ϣ, ���������׸��� */
		for (seg_idx = 1; seg_idx < SSD_SEGS_PER_SEC; ++seg_idx){
			if (write(ssd_vars.fd, &segbi, sizeof(segbi)) < 0) {
				printf("\n\tError: While writing blocks info!!!\
				       Error Num : %d\n", errno);
				return -1;
			}		
		}

		/* д����Ϣ */
		for (seg_idx = 0; seg_idx < SSD_SEGS_PER_SEC; ++seg_idx){
			if (write(ssd_vars.fd, &sit, sizeof(sit)) < 0) {
				printf("\n\tError: While writing segs info!!!\
					Error Num : %d\n", errno);
				return -1;
			}		
		}

		/* дת���� */
		for (seg_idx = 0; seg_idx < SSD_SEGS_PER_SEC; ++seg_idx){
			if (write(ssd_vars.fd, &sec_table, sizeof(sec_table)) < 0) {
				printf("\n\tError: While writing translate table!!!\
				       Error Num : %d\n", errno);
				return -1;
			}		
		}

		/* дת��λͼ */
		if (write(ssd_vars.fd, &sec_map, sizeof(sec_map)) < 0) {
			printf("\n\tError: While writing translate map!!!\
			       Error Num : %d\n", errno);
			return -1;
		}		
		
				offset += SSD_SEGS_PER_SEC * SSD_BLKS_PER_SEG
			* SSD_SECTORS_PER_BLK * SSD_SECTOR_SIZE;
	}

	if (ssd_vars.last_sec == SSD_SEGS_PER_SEC) /* ����Ҫ����������� 1 �� */
		return 0;

	/* ��Ҫ����������� 1 �� */
	offset = SSD_SECTORS_PER_BLK * SSD_SECTOR_SIZE+ (1 << 21);
	offset += (ssd_vars.total_secs - 1) * SSD_SEGS_PER_SEC * SSD_BLKS_PER_SEG
		* SSD_SECTORS_PER_BLK * SSD_SECTOR_SIZE;
	offset += (SSD_SEGS_PER_SEC - 1) * sizeof(segbi);
	
	if (lseek64(ssd_vars.fd, offset, SEEK_SET) < 0) {
		printf("\n\tError: While lseek to the derised location!!!\n");
		return -1;
	}

	/* д����Ϣ */
	for (seg_idx = 0; seg_idx < SSD_SEGS_PER_SEC; ++seg_idx){
		if (seg_idx >= ssd_vars.last_sec){
			sit.stat = cpu_to_le16(SEG_NONEXIST);/* �β����� */
			sit.free_blocks = cpu_to_le32(0); /* ���п��� */
		}

		if (write(ssd_vars.fd, &sit, sizeof(sit)) < 0) {
			printf("\n\tError: While writing segs info!!!\
			       Error Num : %d\n", errno);
			return -1;
		}
	}

	offset += SSD_SEGS_PER_SEC * sizeof(sec_table);
	if (lseek64(ssd_vars.fd, offset, SEEK_SET) < 0) {
		printf("\n\tError: While lseek to the derised location!!!\n");
		return -1;
	}

	/* дת��λͼ */
	for (i = ssd_vars.last_sec * SSD_BLKS_PER_SEG / 8; 
	     i < SSD_SEGS_PER_SEC * SSD_BLKS_PER_SEG / 8; ++i) 
		sec_map.map[i] = 0xFF;
	
	if (write(ssd_vars.fd, &sec_map, sizeof(sec_map)) < 0) {
		printf("\n\tError: While writing translate map!!!\
		       Error Num : %d\n", errno);
		return -1;
	}		

	return 0;
}

/* д������ */
static int ssd_write_sbs()
{
	int i = 0;
	u_int64_t offset = SSD_SECTORS_PER_BLK * SSD_SECTOR_SIZE;/* ������ƫ���� */
	if (lseek64(ssd_vars.fd, offset, SEEK_SET) < 0) {
		printf("\n\tError: While lseek to the super seg location!!!\n");
		return -1;
	}

	/* д������̶����ֵĸ��� 1 */
	if (write(ssd_vars.fd, &sbc, sizeof(sbc)) < 0) {
		printf("\n\tError: While writing sbc 1 !!!\
		       Error Num : %d\n", errno);
		return -1;
	}
	
	if (lseek64(ssd_vars.fd, 2 * offset, SEEK_SET) < 0) {
		printf("\n\tError: While lseek to the super seg location!!!\n");
		return -1;
	}

	/* д������̶����ֵĸ��� 2 */
	if (write(ssd_vars.fd, &sbc, sizeof(sbc)) < 0) {
		printf("\n\tError: While writing sbc 2 !!!\
		       Error Num : %d\n", errno);
		return -1;
	}

	if (lseek64(ssd_vars.fd, 3 * offset, SEEK_SET) < 0) {
		printf("\n\tError: While lseek to the volatile super location!!!\n");
		return -1;
	}
	/* д�������ױ䲿�� */
	if (write(ssd_vars.fd, &sbv, sizeof(sbv)) < 0) {
		printf("\n\tError: While writing translate map!!!\
		       Error Num : %d\n", errno);
		return -1;
	}
	/* ���㳬�����ױ䲿�ֵĸ��� */
	sbv.sb_verion = cpu_to_le32(0);
	for (i = 4; i < SSD_BLKS_PER_SEG ; i += 2) {
		if (write(ssd_vars.fd, &sbv, sizeof(sbv.sb_verion)) < 0) {
			printf("\n\tError: While writing translate map!!!\
			       Error Num : %d\n", errno);
			return -1;
		}

		offset = sizeof(sbv) - sizeof(sbv.sb_verion);
		if (lseek64(ssd_vars.fd, offset, SEEK_CUR) < 0) {
			printf("\n\tError: While lseek to the super seg location!!!\n");
			return -1;
		}
	}
	
	return 0;
}

/* ��ʽ���豸 */
static int ssd_format_device(void)
{
	int err = 0;
	
	err= ssd_prepare_sbs();		/* ��ʼ��������ṹ�� */
	if (err < 0)
		goto exit;

	err = ssd_trim_device();	/* �޼��豸 */
	if (err < 0) {
		printf("\n\tError: Failed to trim whole device!!!\n");
		goto exit;
	}

	err = ssd_init_secs_info();	/* ��ʼ��������Ϣ */
	if (err < 0) {
		printf("\n\tError: Failed to Initialise the SECT info!!!\n");
		goto exit;
	}

	/* д������ */
	err = ssd_write_sbs();
	if (err < 0) {
		printf("\n\tError: Failed to write the Super Block!!!\n");
		goto exit;
	}
exit:
	if (err)
		printf("\n\tError: Could not format the device!!!\n");

	/* ˢ�¿��豸�Ļ��� */
	if (fsync(ssd_vars.fd) < 0)
		printf("\n\tError: Could not conduct fsync!!!\n");

	/* �رտ��豸�ļ� */
	if (close(ssd_vars.fd) < 0)
		printf("\n\tError: Failed to close device file!!!\n");

	return err;
}

int main(int argc, char *argv[])
{
	printf("\nfmc_ssd: formatting ssd volume...\n");
	
	ssd_init_global_vars();			/* ��ʼ��ȫ�ֲ��� */
	
	ssd_parse_options(argc, argv);		/* ���������в��� */

	if(ssd_is_mounted() < 0)		/* �ж� ssd �Ƿ񱻹��� */
		return -1;

	if(ssd_get_device_info() < 0)		/* ��ȡ ssd ��Ϣ */
		return -1;
	
	if(ssd_format_device() < 0)		/* ��ʽ�� ssd */
		return -1;

	printf("Info: format ssd success. \n\n");

	return 0;
}

