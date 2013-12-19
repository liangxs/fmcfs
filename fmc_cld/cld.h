/*
 * fmcfs/fmc_cld/cld.h
 */

#ifndef __LINUX_FS_FMC_CLD_H__
#define __LINUX_FS_FMC_CLD_H__

struct cld_sb_info {
	char		c_name[16];	/* Cloud ���� */
	char		c_appid[1];
	char		c_appkey[1];
	char		c_session[1];
	char		c_secret[1];

	struct cld_operations	*cld_ops;/* Cloud ���������� */
};

struct cld_operations {
	//int (*put)();			/* �ϴ��ļ� */
	//int (*get)();			/* �����ļ� */
	//int (*del)();			/* ɾ���ļ� */
};

#endif	/*__LINUX_FS_FMC_CLD_H__*/

