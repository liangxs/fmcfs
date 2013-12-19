/*
 * fmcfs/fmc_hdd/hdd_namei.c
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * @ Beijing University of Posts and Telecommunications,
 * @ CPU & SoC Center of Tsinghua University.
 * Mainly from linux/fs/ext2/namei.c
 *
 * This program can be redistributed under the terms of the GNU Public License.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/ctype.h>

#include "../fmc_fs.h"
#include "hdd.h"

extern const struct address_space_operations hdd_aops;

/* 读取父目录项对象 */
struct dentry *hdd_get_parent(struct dentry *child)
{
	struct qstr dotdot = {.name = "..", .len = 2};

	/* 根据父目录名字和本 inode, 取得父目录的 ino 编号 */
	unsigned long ino = hdd_inode_by_name(child->d_inode, &dotdot);
	if (!ino)
		return ERR_PTR(-ENOENT);

	/* 由超级块和 ino 得到 inode, 则得到 dentry */
	return d_obtain_alias(hdd_iget(child->d_inode->i_sb, ino));
}

/* 关联 dentry 和 inode, 把非目录文件添加到目录结构中 */
static inline int 
hdd_add_nondir(struct dentry *dentry, struct inode *inode)
{
	int err = hdd_add_link(dentry, inode);/* hdd_dir.c*/
	if (!err) {
		d_instantiate(dentry, inode); /* 关联二者 */
		unlock_new_inode(inode);      /* 解锁, inode 可用了 */
		return 0;
	}

	/* 关联失败 */
	inode_dec_link_count(inode);
	unlock_new_inode(inode);
	iput(inode);
	return err;
}

/* 在目录中查找文件, 返回相应目录项 */
static struct dentry *hdd_lookup(struct inode * dir, 
	struct dentry *dentry, struct nameidata *nd)
{
	struct inode * inode;
	ino_t ino;

	if (dentry->d_name.len > HDD_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	ino = hdd_inode_by_name(dir, &dentry->d_name); /* 在目录 inode 中查找文件 */
	inode = NULL;
	if (ino) {
		inode = hdd_iget(dir->i_sb, ino); /* 读取 inode */
		if (unlikely(IS_ERR(inode))) {
			if (PTR_ERR(inode) == -ESTALE) {
				hdd_msg(dir->i_sb, KERN_ERR, __func__,
					"deleted inode referenced: %lu",
					(unsigned long) ino);
				return ERR_PTR(-EIO);
			} else {
				return ERR_CAST(inode);
			}
		}
	}
	return d_splice_alias(inode, dentry);
}

/* 新建非目录文件 */
static int hdd_create(struct inode * dir, struct dentry * dentry,
		      int mode, struct nameidata *nd)
{
	/* 在 hdd_inode_cachep 中分配内存 */
	struct inode * inode = hdd_new_inode (dir, mode);
	int err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {   /* 记录 3 类操作函数表 */
		inode->i_op = &hdd_file_inode_operations; 
		inode->i_mapping->a_ops = &hdd_aops;
		inode->i_fop = &hdd_file_operations;
		mark_inode_dirty(inode);

		err = hdd_add_nondir(dentry, inode); /* 添加文件目录项 */
	}
	return err;
}

/* 创建硬链接 */
static int hdd_link (struct dentry * old_dentry, 
	struct inode * dir, struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode;
	int err;

	if (inode->i_nlink >= HDD_LINK_MAX)
		return -EMLINK;

	inode->i_ctime = CURRENT_TIME_SEC;
	inode_inc_link_count(inode);
	atomic_inc(&inode->i_count);

	err = hdd_add_link(dentry, inode); /* 添加链接 */
	if (!err) {
		d_instantiate(dentry, inode);
		return 0;
	}

	/* 失败 */
	inode_dec_link_count(inode);
	iput(inode);
	return err;
}

/* 从目录 inode 中删除对应的目录项 */
static int hdd_unlink(struct inode * dir, struct dentry *dentry)
{
	struct inode * inode = dentry->d_inode;
	struct hdd_dir_entry * de;
	struct page * page;
	int err = -ENOENT;

	/* 依次扫描各个目录项，查找目标 */
	de = hdd_find_entry (dir, &dentry->d_name, &page);
	if (!de)
		goto out;

	/* 把目录项从目录表中删除
	( 即设置该目录项的前一项的 rec_len 字段，跳过被删除项 ) */
	err = hdd_delete_entry (de, page);
	if (err)
		goto out;

	inode->i_ctime = dir->i_ctime;
	inode_dec_link_count(inode);
	err = 0;
out:
	return err;
}

/* 创建符号链接 */
static int hdd_symlink (struct inode * dir, struct dentry * dentry,
	const char * symname)
{
	struct super_block * sb = dir->i_sb;
	int err = -ENAMETOOLONG;
	unsigned len = strlen(symname)+1; /* 目标路径长度 */
	struct inode * inode;

	if (len > sb->s_blocksize) /* 符号链接的目标长度不能超过一块 */
		goto out;

	inode = hdd_new_inode (dir, S_IFLNK | S_IRWXUGO); /* 分配 inode */
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out;

	if (len > sizeof (HDD_I(inode)->i_data)) { /* 普通符号链接 */
		inode->i_op = &hdd_symlink_inode_operations;
		inode->i_mapping->a_ops = &hdd_aops;
		err = page_symlink(inode, symname, len);
		if (err)
			goto out_fail;
	} else { /* 快速符号链接 */
		inode->i_op = &hdd_fast_symlink_inode_operations;
		memcpy((char*)(HDD_I(inode)->i_data),symname,len);
		inode->i_size = len-1;
	}
	mark_inode_dirty(inode);

	err = hdd_add_nondir(dentry, inode);
out:
	return err;

out_fail:
	inode_dec_link_count(inode);
	unlock_new_inode(inode);
	iput (inode);
	goto out;
}

/* 创建目录文件 */
static int hdd_mkdir(struct inode * dir,
	struct dentry * dentry, int mode)
{
/* 
dir：	父目录
dentry；新目录的路径名
mode：	新目录的访问模式
*/
	struct inode * inode;
	int err = -EMLINK;

	if (dir->i_nlink >= HDD_LINK_MAX) /* 子文件不能超过最大链接数 */
		goto out;

	inode_inc_link_count(dir); /* 增加链接数 */
	
	inode = hdd_new_inode (dir, S_IFDIR | mode);/* 分配一个新的 inode */
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_dir;

	/* 设置 inode 的文件, inode ,地址空间操作函数表 */
	inode->i_op = &hdd_dir_inode_operations;
	inode->i_fop = &hdd_dir_operations;
	inode->i_mapping->a_ops = &hdd_aops;

	inode_inc_link_count(inode); /* 增加链接个数 */
	
	err = hdd_make_empty(inode, dir); /* 添加. 和 .. 目录项 */
	if (err)
		goto out_fail;

	err = hdd_add_link(dentry, inode); /* 添加到父目录的 inode 中 */
	if (err)
		goto out_fail;

	d_instantiate(dentry, inode); /* 关联二者 */
	unlock_new_inode(inode);
out:
	return err;

out_fail:
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	unlock_new_inode(inode);
	iput(inode);
out_dir:
	inode_dec_link_count(dir);
	goto out;
}

/* 删除目录文件 */
static int hdd_rmdir (struct inode * dir, struct dentry *dentry)
{
	struct inode * inode = dentry->d_inode;
	int err = -ENOTEMPTY;

	if (hdd_empty_dir(inode)) { /* 确保目录为空 */
		err = hdd_unlink(dir, dentry); /* 从父目录中删除此目录项 */
		if (!err) {
			inode->i_size = 0;
			inode_dec_link_count(inode);
			inode_dec_link_count(dir); /* 把指向 inode 的硬链接减 1 */
		}
	}
	return err;
}

/* 创建特殊文件 */
static int hdd_mknod (struct inode * dir, struct dentry *dentry,
		      int mode, dev_t rdev)
{
	struct inode * inode;
	int err;

	if (!new_valid_dev(rdev))
		return -EINVAL;

	inode = hdd_new_inode (dir, mode); /* 新建 inode */
	err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		init_special_inode(inode, inode->i_mode, rdev);
		mark_inode_dirty(inode);
		err = hdd_add_nondir(dentry, inode);
	}

	return err;
}

/* 重命名文件 */
static int hdd_rename (struct inode * old_dir, struct dentry * old_dentry,
	struct inode * new_dir,	struct dentry * new_dentry )
{
	struct inode * old_inode = old_dentry->d_inode;
	struct inode * new_inode = new_dentry->d_inode;
	struct page * dir_page = NULL;
	struct hdd_dir_entry * dir_de = NULL;
	struct page * old_page;
	struct hdd_dir_entry * old_de;
	int err = -ENOENT;

	/* 查找原目录项 */
	old_de = hdd_find_entry (old_dir, &old_dentry->d_name, &old_page);
	if (!old_de)
		goto out;

	if (S_ISDIR(old_inode->i_mode)) { /* 更改目录文件名 */
		err = -EIO;
		dir_de = hdd_dotdot(old_inode, &dir_page);
		if (!dir_de)
			goto out_old;
	}

	if (new_inode) {
		struct page *new_page;
		struct hdd_dir_entry *new_de;

		err = -ENOTEMPTY;
		if (dir_de && !hdd_empty_dir (new_inode))/* 空目录, 直接 kunmap */
			goto out_dir;

		err = -ENOENT;
		new_de = hdd_find_entry (new_dir,  /* 从 dentry 中查找目录项 */
			&new_dentry->d_name, &new_page);
		if (!new_de)
			goto out_dir;

		inode_inc_link_count(old_inode);
		hdd_set_link(new_dir, new_de, new_page, old_inode, 1);
		new_inode->i_ctime = CURRENT_TIME_SEC;

		if (dir_de)
			drop_nlink(new_inode);
		inode_dec_link_count(new_inode);
	} else {
		if (dir_de) {
			err = -EMLINK;
			if (new_dir->i_nlink >= HDD_LINK_MAX)
				goto out_dir;
		}
		inode_inc_link_count(old_inode);
		err = hdd_add_link(new_dentry, old_inode);
		if (err) {
			inode_dec_link_count(old_inode);
			goto out_dir;
		}
		if (dir_de)
			inode_inc_link_count(new_dir);
	}

	/* set the ctime for inodes on a rename */
	old_inode->i_ctime = CURRENT_TIME_SEC;

	hdd_delete_entry (old_de, old_page);
	inode_dec_link_count(old_inode);

	if (dir_de) {
		if (old_dir != new_dir)
			hdd_set_link(old_inode, dir_de, dir_page, new_dir, 0);
		else {
			kunmap(dir_page);
			page_cache_release(dir_page);
		}
		inode_dec_link_count(old_dir);
	}
	return 0;


out_dir:
	if (dir_de) {
		kunmap(dir_page);
		page_cache_release(dir_page);
	}
out_old:
	kunmap(old_page);
	page_cache_release(old_page);
out:
	return err;
}

const struct inode_operations hdd_dir_inode_operations = {
	.create		= hdd_create,
	.lookup		= hdd_lookup,
	.link		= hdd_link,
	.unlink		= hdd_unlink,
	.symlink	= hdd_symlink,
	.mkdir		= hdd_mkdir,
	.rmdir		= hdd_rmdir,
	.mknod		= hdd_mknod,
	.rename		= hdd_rename,
	.setattr	= hdd_setattr,
};

const struct inode_operations hdd_special_inode_operations = {
	.setattr	= hdd_setattr,
};
