#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs_context.h>
#include <linux/pagemap.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/fs.h>

#include "zipr/zipr.h"
#include "zipContent/zipContent.h"

#define MOUNTZIP_MAGIC 0x4D5A4950  /* 'M' 'Z' 'I' 'P' */

static struct dentry *mountzip_lookup(struct inode *directory, struct dentry *dentry, unsigned int flags);
static int mountzip_readdir(struct file *file, struct dir_context *context);

struct MountzipInfo {
	struct Zipr *zip;
	struct ZipContent content;
};

struct MountzipInode {
	struct mutex lock;
	void *data;
	loff_t dataLength;
	struct inode vfs_inode;
};

static struct kmem_cache *mountzip_inode_cachep;

static inline struct MountzipInode *MOUNTZIP_I(struct inode *inode) {
	return container_of(inode, struct MountzipInode, vfs_inode);
}

static inline const struct Element *elementForInode(struct MountzipInfo *info, struct inode *inode) {
	return zipContentElement(&info->content, inode->i_ino - 1);
}

static int mountzip_read_data(struct inode *vfs_inode) {
	struct MountzipInode *inode = MOUNTZIP_I(vfs_inode);
	if (inode->data)
		return 0;
	int result = 0;
	inode->dataLength = i_size_read(vfs_inode);
	inode->data = kvmalloc(inode->dataLength, GFP_KERNEL);
	if (!inode->data) {
		result = -ENOMEM;
		goto ERROR;
	}
	struct MountzipInfo *info = vfs_inode->i_sb->s_fs_info;
	const struct Element *element = zipContentElement(&info->content, vfs_inode->i_ino - 1);
	struct ZiprEntry entry;
	result = ziprEntryAt(info->zip,
		element->position,
		&entry,
		NULL,
		0);
	if (result < 0)
		goto ERROR;
	struct ZiprFile *zf;
	result = ziprFopen(info->zip, &entry, &zf);
	if (result < 0)
		goto ERROR;
	ssize_t bytes = ziprFread(zf, inode->data, inode->dataLength);
	ziprFclose(zf);
	if (bytes < 0) {
		result = bytes;
		goto ERROR;
	}
	inode->dataLength = bytes;
	return 0;

ERROR:	kvfree(inode->data);
	inode->data = NULL;
	inode->dataLength = 0;
	return result;
}

static int mountzip_read_folio(struct file *file, struct folio *folio) {
	struct inode *vfs_inode = folio->mapping->host;
	struct MountzipInode *inode = MOUNTZIP_I(vfs_inode);
	int result =  mutex_lock_interruptible(&inode->lock);
	if (result < 0)
		goto END;
	result = mountzip_read_data(vfs_inode);
	if (result < 0)
		goto MUTEX;
	loff_t position = folio_pos(folio);
	size_t size = folio_size(folio);
	size_t valid = 0;
	if (position < inode->dataLength) {
		valid = min_t(loff_t, size, inode->dataLength - position);
		memcpy_to_folio(folio, 0, (char*)inode->data + position, valid);
	}
	if (valid < size)
		folio_zero_range(folio, valid, size - valid);
	folio_mark_uptodate(folio);
MUTEX:	mutex_unlock(&inode->lock);
END:	folio_unlock(folio);
	return result;
}

static const struct address_space_operations mountzip_aops = {
	.read_folio = mountzip_read_folio
};

static const struct inode_operations mountzip_dir_ops = {
	.lookup = mountzip_lookup
};

static const struct file_operations mountzip_dir_fops = {
	.read = generic_read_dir,
	.iterate_shared = mountzip_readdir,
	.llseek = generic_file_llseek
};

static const struct file_operations mountzip_file_fops = {
	.owner = THIS_MODULE,
	.read_iter = generic_file_read_iter,
	.llseek = generic_file_llseek,
	.mmap = generic_file_readonly_mmap
};

static void mountzip_inode_init_once(void *data) {
	struct MountzipInode *inode = data;
	inode_init_once(&inode->vfs_inode);
}

static struct inode *mountzip_iget(struct super_block *sb, uint32_t index) {
	struct inode *inode = iget_locked(sb, index + 1);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;
	simple_inode_init_ts(inode);
	const struct MountzipInfo *info = sb->s_fs_info;
	const struct Element *element = zipContentElement(&info->content, index);
	if (zipContentIsDirectory(element)) {
		inode->i_mode = S_IFDIR | 0555;
		inode->i_op = &mountzip_dir_ops;
		inode->i_fop = &mountzip_dir_fops;
	}
	else {
		inode->i_mode = S_IFREG | 0444;
		inode->i_fop = &mountzip_file_fops;
		inode->i_mapping->a_ops = &mountzip_aops;
		struct ZiprEntry entry;
		int result = ziprEntryAt(info->zip,
			element->position,
			&entry,
			NULL,
			0);
		if (result < 0) {
			iget_failed(inode);
			return ERR_PTR(result);
		}
		i_size_write(inode, entry.size);
		inode_set_bytes(inode, entry.size);
	}
	unlock_new_inode(inode);
	return inode;
}

static struct dentry *mountzip_lookup(struct inode *directory, struct dentry *dentry, unsigned int flags) {
	struct inode *result = NULL;
	struct super_block *sb = directory->i_sb;
	struct MountzipInfo *info = sb->s_fs_info;
	const struct Element *found;
	const struct Element *element = zipContentElement(&info->content, directory->i_ino - 1);
	if (!zipContentLookup(&info->content,
		element,
		dentry->d_name.name,
		dentry->d_name.len,
		&found))
		result = mountzip_iget(sb, zipContentIndex(&info->content, found));
	return d_splice_alias(result, dentry);
}

static int mountzip_readdir(struct file *file, struct dir_context *context) {
	if (!dir_emit_dots(file, context))
		return 0;
	struct inode *directory = file_inode(file);
	struct super_block *sb = directory->i_sb;
	struct MountzipInfo *info = sb->s_fs_info;
	struct ZipContent *content = &info->content;
	const struct Element *element = zipContentElement(content, directory->i_ino - 1);
	const struct Element *child = zipContentChildren(content, element);
	loff_t skip = context->pos - 2;
	if (skip >= element->directory.count)
		return 0;
	child += skip;
	for (uint32_t i = element->directory.count - skip; i != 0; i--) {
		if (!dir_emit(context,
			zipContentName(content, child),
			child->nameLength,
			zipContentIndex(content, child) + 1,
			zipContentIsDirectory(child) ? DT_DIR : DT_REG))
			break;
		child++;
		context->pos++;
	}
	return 0;
}

static struct inode *mountzip_alloc_inode(struct super_block *sb) {
	struct MountzipInode *inode;
	inode = alloc_inode_sb(sb, mountzip_inode_cachep, GFP_KERNEL);
	if (!inode)
		return NULL;
	mutex_init(&inode->lock);
	inode->data = NULL;
	inode->dataLength = 0;
	return &inode->vfs_inode;
}

static void mountzip_free_inode(struct inode *inode) {
	struct MountzipInode *tofree = MOUNTZIP_I(inode);
	if (tofree->data) {
		kvfree(tofree->data);
		tofree->data = NULL;
	}
	kmem_cache_free(mountzip_inode_cachep, tofree);
}

static const struct super_operations mountzip_super_operations = {
	.alloc_inode = mountzip_alloc_inode,
	.free_inode = mountzip_free_inode,
	.statfs = simple_statfs
};

static int mountzip_fill_super(struct super_block *sb, struct fs_context *fc) {
	if (!fc->source)
		return invalfc(fc, "no zip file");
	struct MountzipInfo *info;
	info = kzalloc(sizeof(struct MountzipInfo), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	sb->s_fs_info = info;
	int result;
	result = ziprOpen(fc->source, &info->zip);
	if (result < 0)
		return result;
	result = zipContentBuild(&info->content, info->zip);
	if (result < 0)
		return result;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_magic = MOUNTZIP_MAGIC;
	sb->s_flags |= SB_RDONLY | SB_NOATIME;
	sb->s_op = &mountzip_super_operations;

	struct inode *root = mountzip_iget(sb, ZIP_CONTENT_ROOT);
	if (IS_ERR(root))
		return PTR_ERR(root);
	sb->s_root = d_make_root(root);
	if (!sb->s_root)
		return -ENOMEM;
	return 0;
}

static void mountzip_kill_sb(struct super_block *sb) {
	struct MountzipInfo *info = sb->s_fs_info;
	kill_anon_super(sb);
	if (info) {
		ziprClose(info->zip);
		zipContentDestroy(&info->content);
		kfree(info);
	}
}

static int mountzip_get_tree(struct fs_context *fc) {
	return get_tree_nodev(fc, mountzip_fill_super);
}

static const struct fs_context_operations mountzip_context_operations = {
	.get_tree = mountzip_get_tree
};

static int mountzip_init_fs_context(struct fs_context *fc) {
	fc->ops = &mountzip_context_operations;
	return 0;
}

static struct file_system_type mountzip_fs_type = {
	.owner = THIS_MODULE,
	.name = "mountzip",
	.init_fs_context = mountzip_init_fs_context,
	.kill_sb = mountzip_kill_sb
};

static int __init mountzip_init(void) {
	mountzip_inode_cachep = kmem_cache_create("mountzip_inode",
		sizeof(struct MountzipInode),
		0,
		SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
		mountzip_inode_init_once);
	if (!mountzip_inode_cachep)
		return -ENOMEM;
	int result = register_filesystem(&mountzip_fs_type);
	if (result < 0)
		kmem_cache_destroy(mountzip_inode_cachep);
	return result;
}

static void __exit mountzip_exit(void) {
	unregister_filesystem(&mountzip_fs_type);
	rcu_barrier();
	kmem_cache_destroy(mountzip_inode_cachep);
}

module_init(mountzip_init);
module_exit(mountzip_exit);
MODULE_LICENSE("Dual BSD/GPL");
