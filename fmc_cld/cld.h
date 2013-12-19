/*
 * fmcfs/fmc_cld/cld.h
 */

#ifndef __LINUX_FS_FMC_CLD_H__
#define __LINUX_FS_FMC_CLD_H__

struct cld_sb_info {
	char		c_name[16];	/* Cloud 名称 */
	char		c_appid[1];
	char		c_appkey[1];
	char		c_session[1];
	char		c_secret[1];

	struct cld_operations	*cld_ops;/* Cloud 操作函数表 */
};

struct cld_operations {
	//int (*put)();			/* 上传文件 */
	//int (*get)();			/* 下载文件 */
	//int (*del)();			/* 删除文件 */
};

#endif	/*__LINUX_FS_FMC_CLD_H__*/

