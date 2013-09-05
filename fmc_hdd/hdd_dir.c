/*
 * fmcfs/fmc_hdd/hdd_dir.c
 * 
 * Copyright (C) 2013 by Xuesen Liang, <liangxuesen@gmail.com>
 * @ Beijing University of Posts and Telecommunications,
 * @ CPU & SoC Center of Tsinghua University.
 *
 * This program can be redistributed under the terms of the GNU Public License.
 */

#include <linux/buffer_head.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include "hdd.h"

static unsigned char hdd_filetype_table[HDD_FT_MAX] = {
	[HDD_FT_UNKNOWN]	= DT_UNKNOWN,
	[HDD_FT_REG_FILE]	= DT_REG,
	[HDD_FT_DIR]		= DT_DIR,
	[HDD_FT_CHRDEV]		= DT_CHR,
	[HDD_FT_BLKDEV]		= DT_BLK,
	[HDD_FT_FIFO]		= DT_FIFO,
	[HDD_FT_SOCK]		= DT_SOCK,
	[HDD_FT_SYMLINK]	= DT_LNK,
};

/* 目录项中的文件类型 */
#define S_SHIFT 12
static unsigned char hdd_type_by_mode[S_IFMT >> S_SHIFT] = {
	[S_IFREG >> S_SHIFT]	= HDD_FT_REG_FILE,
	[S_IFDIR >> S_SHIFT]	= HDD_FT_DIR,
	[S_IFCHR >> S_SHIFT]	= HDD_FT_CHRDEV,
	[S_IFBLK >> S_SHIFT]	= HDD_FT_BLKDEV,
	[S_IFIFO >> S_SHIFT]	= HDD_FT_FIFO,
	[S_IFSOCK >> S_SHIFT]	= HDD_FT_SOCK,
	[S_IFLNK >> S_SHIFT]	= HDD_FT_SYMLINK,
};

/* 释放一页 */
static inline void hdd_put_page(struct page *page)
{
	kunmap(page);
	page_cache_release(page);
}

/* 根据目录 inode 和文件名, 取得文件的 ino 编号 */
ino_t hdd_inode_by_name(struct inode *dir, struct qstr *child)
{
	ino_t res = 0;
	struct hdd_dir_entry *de;
	struct page *page;

	de = hdd_find_entry (dir, child, &page);
	if (de) {
		res = le32_to_cpu(de->inode);
		hdd_put_page(page);
	}
	return res;
}

/* 块大小 */
static inline unsigned hdd_chunk_size(struct inode *inode)
{
	return inode->i_sb->s_blocksize;
}

/* 目录文件的(上取整)页数 */
static inline unsigned long dir_pages(struct inode *inode)
{
	return (inode->i_size+PAGE_CACHE_SIZE-1)>>PAGE_CACHE_SHIFT;
}

/* 从磁盘上读取记录长度 */
static inline unsigned hdd_rec_len_from_disk(__le16 dlen)
{
	unsigned len = le16_to_cpu(dlen);

	if (len == HDD_MAX_REC_LEN)
		return 1 << 16;
	return len;
}

/* 把记录长度写到磁盘上 */
static inline __le16 hdd_rec_len_to_disk(unsigned len)
{
	if (len == (1 << 16))
		return cpu_to_le16(HDD_MAX_REC_LEN);
	else
		BUG_ON(len > (1 << 16));
	return cpu_to_le16(len);
}

/* 检查目录文件一页内容 */
static void hdd_check_page(struct page *page, int quiet)
{
	struct inode *dir = page->mapping->host; /* 目录文件的 inode */
	struct super_block *sb = dir->i_sb;
	unsigned chunk_size = hdd_chunk_size(dir);
	char *kaddr = page_address(page);  /* 页中数据地址 */
	u32 max_inumber = HDD_SB(sb)->inodes_count;
	hdd_dirent *p = NULL;
	unsigned limit = PAGE_CACHE_SIZE;
	unsigned offs, rec_len;
	char *error;

	/* 若是最后一页, 检查是否为页的整数倍 */
	if ((dir->i_size >> PAGE_CACHE_SHIFT) == page->index) {
		/* limit 为最后一页中的数据长度, 应为 0 */
		limit = dir->i_size & ~PAGE_CACHE_MASK; /*(~(PAGE_SIZE-1))*/
		if (limit & (chunk_size - 1))
			goto Ebadsize;
		if (!limit)
			goto out;
	}

	for (offs = 0; offs <= limit - HDD_DIR_REC_LEN(1); offs += rec_len) {
		p = (hdd_dirent *)(kaddr + offs);
		rec_len = hdd_rec_len_from_disk(p->rec_len); /* 记录的长度 */

		if (rec_len < HDD_DIR_REC_LEN(1)) /* 文件名小于 1 字节 */
			goto Eshort;
		if (rec_len & 3)	/* 没有对齐 4 字节 */
			goto Ealign;
		if (rec_len < HDD_DIR_REC_LEN(p->name_len)) /* 小于文件名 */
			goto Enamelen;
		if (((offs + rec_len - 1) ^ offs) & ~(chunk_size-1))
			goto Espan; /* 扩展文件 */
		if (le32_to_cpu(p->inode) >= max_inumber) /* inode 号太大 */
			goto Einumber;
	}
	if (offs != limit)
		goto Eend;
out:
	SetPageChecked(page); /* 缓存 page */
	return;

	/* Too bad, we had an error */
Ebadsize:
	if (!quiet)
		hdd_msg(sb, KERN_ERR, __func__,
		"size of directory #%lu is not a multiple "
		"of chunk size", dir->i_ino);
	goto fail;
Eshort:
	error = "rec_len is smaller than minimal";
	goto bad_entry;
Ealign:
	error = "unaligned directory entry";
	goto bad_entry;
Enamelen:
	error = "rec_len is too small for name_len";
	goto bad_entry;
Espan:
	error = "directory entry across blocks";
	goto bad_entry;
Einumber:
	error = "inode out of bounds";
bad_entry:
	if (!quiet)
		hdd_msg(sb, KERN_ERR, __func__, 
		"bad entry in directory #%lu: : %s - "
		"offset=%lu, inode=%lu, rec_len=%d, name_len=%d",
		dir->i_ino, error, (page->index<<PAGE_CACHE_SHIFT)+offs,
		(unsigned long) le32_to_cpu(p->inode),
		rec_len, p->name_len);
	goto fail;
Eend:
	if (!quiet) {
		p = (hdd_dirent *)(kaddr + offs);
		hdd_msg(sb, KERN_ERR, "hdd_check_page",
			"entry in directory #%lu spans the page boundary"
			"offset=%lu, inode=%lu",
			dir->i_ino, (page->index<<PAGE_CACHE_SHIFT)+offs,
			(unsigned long) le32_to_cpu(p->inode));
	}
fail:
	SetPageChecked(page);
	SetPageError(page);
}

/* 读取目录文件的一页 */
static struct page * hdd_get_page(struct inode *dir,
				  unsigned long n, int quiet)
{
	struct address_space *mapping = dir->i_mapping;

	/* 读一页, 调用 a_ops->readpage() 从设备读取页, 对于超过文件的一页, 则分配之 */
	struct page *page = read_mapping_page(mapping, n, NULL);
	if (!IS_ERR(page)) {
		kmap(page);
		if (!PageChecked(page))
			hdd_check_page(page, quiet); /* 检查页内容 */
		if (PageError(page))
			goto fail;
	}
	return page;
fail:
	hdd_put_page(page);
	return ERR_PTR(-EIO);
}

/* 页长度 */
static unsigned hdd_last_byte(struct inode *inode, unsigned long page_nr)
{
	unsigned last_byte = inode->i_size;

	/* 若非最后一页,返回页长, 否则返回最后一页中的数据长度 */
	last_byte -= page_nr << PAGE_CACHE_SHIFT;
	if (last_byte > PAGE_CACHE_SIZE) /*  */
		last_byte = PAGE_CACHE_SIZE;
	return last_byte;
}

/* 检查是否为对应目录项. 是则返回 1, 不是则返回 0 */
static inline int hdd_match (int len, const char * const name,
			     struct hdd_dir_entry * de)
{
	if (len != de->name_len) /* 长度不同 */
		return 0;
	if (!de->inode)		/* inode 编号为 0 */
		return 0;
	return !memcmp(name, de->name, len); /* 比较文件名 */
}

/* 提交目录文件的页 */
static int hdd_commit_chunk(struct page *page, loff_t pos, unsigned len)
{
	struct address_space *mapping = page->mapping;
	struct inode *dir = mapping->host;
	int err = 0;

	dir->i_version++;
	block_write_end(NULL, mapping, pos, len, len, page, NULL);

	if (pos+len > dir->i_size) {
		i_size_write(dir, pos+len);
		mark_inode_dirty(dir);
	}

	if (IS_DIRSYNC(dir)) {
		err = write_one_page(page, 1);
		if (!err)
			err = hdd_sync_inode(dir);
	} else {
		unlock_page(page);
	}

	return err;
}

/* 把目录项添加到父目录文件中 */
int hdd_add_link (struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = dentry->d_parent->d_inode; /* 父目录文件的 inode */
	const char *name = dentry->d_name.name;     /* 文件名 */
	int namelen = dentry->d_name.len;	    /* 文件名长度 */
	unsigned reclen = HDD_DIR_REC_LEN(namelen); /* 记录的长度 */

	unsigned chunk_size = hdd_chunk_size(dir);  /* 块大小 */
	unsigned short rec_len = 0, name_len = 0;
	struct page *page = NULL;
	hdd_dirent * de = NULL;
	unsigned long npages = dir_pages(dir);  /* 目录文件的页数 */
	unsigned long n = 0;
	char *kaddr = NULL;
	loff_t pos = 0;
	int err = 0;

	for (n = 0; n <= npages; n++) { /* 处理每个页, 并在需要时扩展目录 */
		char *dir_end = NULL;
		
		/* 读取一页并检查之, 若超过最后一页, 则扩展之 */
		page = hdd_get_page(dir, n, 0);
		err = PTR_ERR(page);
		if (IS_ERR(page))
			goto out;

		lock_page(page);
		kaddr = page_address(page);
		dir_end = kaddr + hdd_last_byte(dir, n); /* 末端 */
		de = (hdd_dirent *)kaddr; /* 起始 */
		kaddr += PAGE_CACHE_SIZE - reclen; /* 到终点的距离 */
		while ((char *)de <= kaddr) {
			if ((char *)de == dir_end) {
				/* We hit i_size */
				name_len = 0;
				rec_len = chunk_size;
				de->rec_len = hdd_rec_len_to_disk(chunk_size);
				de->inode = 0;
				goto got_it; /* 需要新分配块 */
			}

			if (de->rec_len == 0) { /* 存在长度为 0 的目录项 */
				hdd_msg(dir->i_sb, KERN_ERR, __func__,
					"zero-length directory entry");
				err = -EIO;
				goto out_unlock;
			}

			err = -EEXIST;
			if (hdd_match (namelen, name, de)) /* 目录项已存在 */
				goto out_unlock;

			name_len = HDD_DIR_REC_LEN(de->name_len);
			rec_len = hdd_rec_len_from_disk(de->rec_len);
			if (!de->inode && rec_len >= reclen) /* 可用的空闲目录项 */
				goto got_it;

			if (rec_len >= name_len + reclen) /* 目录项中有足够空闲空间 */
				goto got_it;

			de = (hdd_dirent *) ((char *) de + rec_len);
		} /* while */
		unlock_page(page);
		hdd_put_page(page);
	} /* for */
	BUG();
	return -EINVAL;

got_it:
	pos = page_offset(page) +
		(char*)de - (char*)page_address(page);
	/* 准备把目录项写到 page 中 */
	err = __hdd_write_begin(NULL, page->mapping, pos,
				rec_len, 0, &page, NULL);
	if (err)
		goto out_unlock;

	if (de->inode) { /* 若是分配在原有效目录项中的空闲部分 */
		hdd_dirent *de1 = (hdd_dirent *) ((char *) de + name_len);
		de1->rec_len = hdd_rec_len_to_disk(rec_len - name_len);
		de->rec_len = hdd_rec_len_to_disk(name_len); /* 目录项长度 */
		de = de1;
	}

	/* 写目录项 */
	de->name_len = namelen; /* 文件名长度 */
	memcpy(de->name, name, namelen); /* 拷贝文件名 */
	de->inode = cpu_to_le32(inode->i_ino); /* inode 编号 */
	de->file_type = hdd_type_by_mode[(inode->i_mode & S_IFMT)>>S_SHIFT];

	err = hdd_commit_chunk(page, pos, rec_len); /* 提交目录文件的页 */

	dir->i_mtime = dir->i_ctime = CURRENT_TIME_SEC;
	HDD_I(dir)->i_flags &= ~FS_BTREE_FL; /* 标记不是 B 树 */
	mark_inode_dirty(dir);
	/* OFFSET_CACHE */
out_put:
	hdd_put_page(page);
out:
	return err;
out_unlock:
	unlock_page(page);
	goto out_put;
}

/* 返回 p 下一项的目录项地址 */
static inline hdd_dirent *hdd_next_entry(hdd_dirent *p)
{
	return (hdd_dirent *)((char *)p +
		hdd_rec_len_from_disk(p->rec_len));
}

/* 扫描目录项, 查找目标 */
struct hdd_dir_entry *hdd_find_entry (struct inode * dir,
	struct qstr *child, struct page ** res_page)
{
	const char *name = child->name;
	int namelen = child->len;
	unsigned reclen = HDD_DIR_REC_LEN(namelen);
	unsigned long start, n;
	unsigned long npages = dir_pages(dir); /* 目录文件页数 */
	struct page *page = NULL;
	struct hdd_inode_info *hi = HDD_I(dir);
	hdd_dirent * de;
	int dir_has_error = 0;

	if (npages == 0)
		goto out;

	*res_page = NULL;

	start = hi->i_dir_start_lookup;
	if (start >= npages)
		start = 0;
	n = start;

	do {
		char *kaddr;
		page = hdd_get_page(dir, n, dir_has_error);
		if (!IS_ERR(page)) {
			kaddr = page_address(page);
			de = (hdd_dirent *) kaddr;
			kaddr += hdd_last_byte(dir, n) - reclen;
			while ((char *) de <= kaddr) {
				if (de->rec_len == 0) {
					hdd_msg(dir->i_sb, KERN_ERR, __func__,
						"zero-length directory entry");
					hdd_put_page(page);
					goto out;
				}
				if (hdd_match (namelen, name, de))
					goto found;
				de = hdd_next_entry(de);
			}
			hdd_put_page(page);
		} else
			dir_has_error = 1;

		if (++n >= npages)
			n = 0;
		/* next page is past the blocks we've got */
		if (unlikely(n > dir->i_blocks)) {
			hdd_msg(dir->i_sb, KERN_ERR, __func__,
				"dir %lu size %lld exceeds block count %llu",
				dir->i_ino, dir->i_size,
				(unsigned long long)dir->i_blocks);
			goto out;
		}
	} while (n != start);
out:
	return NULL;

found:
	*res_page = page;
	hi->i_dir_start_lookup = n;
	return de;
}

/* 删除目录项 */
int hdd_delete_entry (struct hdd_dir_entry * dir, struct page * page )
{
	struct address_space *mapping = page->mapping;
	struct inode *inode = mapping->host;
	char *kaddr = page_address(page); /* 页起始地址 */

	unsigned from = ((char*)dir - kaddr) & ~(hdd_chunk_size(inode)-1);
	unsigned to = ((char *)dir - kaddr) +
				hdd_rec_len_from_disk(dir->rec_len);
	loff_t pos;
	hdd_dirent * pde = NULL; /* 前一个 de */
	hdd_dirent * de = (hdd_dirent *) (kaddr + from);
	int err;

	while ((char*)de < (char*)dir) {
		if (de->rec_len == 0) {
			hdd_msg(inode->i_sb, KERN_ERR, __func__,
				"zero-length directory entry");
			err = -EIO;
			goto out;
		}
		pde = de;
		de = hdd_next_entry(de);
	}

	if (pde)
		from = (char*)pde - (char*)page_address(page);
	pos = page_offset(page) + from;
	lock_page(page);
	/* 准备 */
	err = __hdd_write_begin(NULL, page->mapping, pos, to - from, 0,
							&page, NULL);
	BUG_ON(err);
	if (pde)
		pde->rec_len = hdd_rec_len_to_disk(to - from);
	dir->inode = 0;
	err = hdd_commit_chunk(page, pos, to - from);
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	HDD_I(inode)->i_flags &= ~FS_BTREE_FL;
	mark_inode_dirty(inode);
out:
	hdd_put_page(page);
	return err;
}

int hdd_make_empty(struct inode *inode, struct inode *parent)
{
	struct address_space *mapping = inode->i_mapping;
	struct page *page = grab_cache_page(mapping, 0);
	unsigned chunk_size = hdd_chunk_size(inode);
	struct hdd_dir_entry * de;
	int err;
	void *kaddr;

	if (!page)
		return -ENOMEM;

	err = __hdd_write_begin(NULL, page->mapping, 0, chunk_size, 0,
		&page, NULL);
	if (err) {
		unlock_page(page);
		goto fail;
	}
	kaddr = kmap_atomic(page, KM_USER0);
	memset(kaddr, 0, chunk_size);
	de = (struct hdd_dir_entry *)kaddr;
	de->name_len = 1;
	de->rec_len = hdd_rec_len_to_disk(HDD_DIR_REC_LEN(1));
	memcpy (de->name, ".\0\0", 4); /* 本目录 */
	de->inode = cpu_to_le32(inode->i_ino);
	de->file_type = hdd_type_by_mode[(inode->i_mode & S_IFMT)>>S_SHIFT]

	de = (struct hdd_dir_entry *)(kaddr + HDD_DIR_REC_LEN(1));
	de->name_len = 2;
	de->rec_len = hdd_rec_len_to_disk(chunk_size - HDD_DIR_REC_LEN(1));
	de->inode = cpu_to_le32(parent->i_ino);
	memcpy (de->name, "..\0", 4); /* 父目录 */
	de->file_type = hdd_type_by_mode[(inode->i_mode & S_IFMT)>>S_SHIFT]
	kunmap_atomic(kaddr, KM_USER0);
	err = hdd_commit_chunk(page, 0, chunk_size);
fail:
	page_cache_release(page);
	return err;
}

/* 检查目录文件是否为空 */
int hdd_empty_dir (struct inode * inode)
{
	struct page *page = NULL;
	unsigned long i, npages = dir_pages(inode);
	int dir_has_error = 0;

	for (i = 0; i < npages; i++) {
		char *kaddr;
		hdd_dirent * de;
		page = hdd_get_page(inode, i, dir_has_error);

		if (IS_ERR(page)) {
			dir_has_error = 1;
			continue;
		}

		kaddr = page_address(page);
		de = (hdd_dirent *)kaddr;
		kaddr += hdd_last_byte(inode, i) - HDD_DIR_REC_LEN(1);

		while ((char *)de <= kaddr) {
			if (de->rec_len == 0) {
				hdd_msg(inode->i_sb, KERN_ERR, __func__,
					"zero-length directory entry");
				printk("kaddr=%p, de=%p\n", kaddr, de);
				goto not_empty;
			}
			if (de->inode != 0) {
				/* check for . and .. */
				if (de->name[0] != '.')
					goto not_empty;
				if (de->name_len > 2)
					goto not_empty;
				if (de->name_len < 2) {
					if (de->inode !=
						cpu_to_le32(inode->i_ino))
						goto not_empty;
				} else if (de->name[1] != '.')
					goto not_empty;
			}
			de = hdd_next_entry(de);
		}
		hdd_put_page(page);
	}
	return 1;

not_empty:
	hdd_put_page(page);
	return 0;
}

/* 读取目录文件的根目录项 */
struct hdd_dir_entry * hdd_dotdot (struct inode *dir, struct page **p)
{
	struct page *page = hdd_get_page(dir, 0, 0); /* 读取文件首页 */
	hdd_dirent *de = NULL;

	if (!IS_ERR(page)) {
		de = hdd_next_entry((hdd_dirent *) page_address(page));
		*p = page;
	}

	return de;
}

void hdd_set_link(struct inode *dir, struct hdd_dir_entry *de,
	struct page *page, struct inode *inode, int update_times)
{
	loff_t pos = page_offset(page) +
		(char *) de - (char *) page_address(page);
	unsigned len = hdd_rec_len_from_disk(de->rec_len);
	int err;

	lock_page(page);
	err = __hdd_write_begin(NULL, page->mapping, pos, len,
		AOP_FLAG_UNINTERRUPTIBLE, &page, NULL);
	BUG_ON(err);
	de->inode = cpu_to_le32(inode->i_ino);
	de->file_type = hdd_type_by_mode[(inode->i_mode & S_IFMT)>>S_SHIFT];
	err = hdd_commit_chunk(page, pos, len);
	hdd_put_page(page);

	if (update_times)
		dir->i_mtime = dir->i_ctime = CURRENT_TIME_SEC;
	HDD_I(dir)->i_flags &= ~FS_BTREE_FL;
	mark_inode_dirty(dir);
}

static inline unsigned 
hdd_validate_entry(char *base, unsigned offset, unsigned mask)
{
	hdd_dirent *de = (hdd_dirent*)(base + offset);
	hdd_dirent *p  = (hdd_dirent*)(base + (offset&mask));
	while ((char*)p < (char*)de) {
		if (p->rec_len == 0)
			break;
		p = hdd_next_entry(p);
	}
	return (char *)p - base;
}

static int
hdd_readdir (struct file * filp, void * dirent, filldir_t filldir)
{
	loff_t pos = filp->f_pos;
	struct inode *inode = filp->f_path.dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	unsigned int offset = pos & ~PAGE_CACHE_MASK;
	unsigned long n = pos >> PAGE_CACHE_SHIFT;
	unsigned long npages = dir_pages(inode);
	unsigned chunk_mask = ~(hdd_chunk_size(inode)-1);
	unsigned char *types = NULL;
	int need_revalidate = filp->f_version != inode->i_version;

	if (pos > inode->i_size - HDD_DIR_REC_LEN(1))
		return 0;

	types = hdd_filetype_table;

	for ( ; n < npages; n++, offset = 0) {
		char *kaddr, *limit;
		hdd_dirent *de;
		struct page *page = hdd_get_page(inode, n, 0);

		if (IS_ERR(page)) {
			hdd_msg(sb, KERN_ERR, __func__, "bad page in #%lu",
				inode->i_ino);
			filp->f_pos += PAGE_CACHE_SIZE - offset;
			return PTR_ERR(page);
		}
		kaddr = page_address(page);
		if (unlikely(need_revalidate)) {
			if (offset) {
				offset = hdd_validate_entry(kaddr, offset, chunk_mask);
				filp->f_pos = (n<<PAGE_CACHE_SHIFT) + offset;
			}
			filp->f_version = inode->i_version;
			need_revalidate = 0;
		}
		de = (hdd_dirent *)(kaddr+offset);
		limit = kaddr + hdd_last_byte(inode, n) - HDD_DIR_REC_LEN(1);
		for ( ;(char*)de <= limit; de = hdd_next_entry(de)) {
			if (de->rec_len == 0) {
				hdd_msg(sb, KERN_ERR, __func__,
					"zero-length directory entry");
				hdd_put_page(page);
				return -EIO;
			}
			if (de->inode) {
				int over;
				unsigned char d_type = DT_UNKNOWN;

				if (types && de->file_type < HDD_FT_MAX)
					d_type = types[de->file_type];

				offset = (char *)de - kaddr;
				over = filldir(dirent, de->name, de->name_len,
						(n<<PAGE_CACHE_SHIFT) | offset,
						le32_to_cpu(de->inode), d_type);
				if (over) {
					hdd_put_page(page);
					return 0;
				}
			}
			filp->f_pos += hdd_rec_len_from_disk(de->rec_len);
		}
		hdd_put_page(page);
	}
	return 0;
}

const struct file_operations hdd_dir_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.readdir	= hdd_readdir,
	.unlocked_ioctl = hdd_ioctl,
	.fsync		= simple_fsync,
};