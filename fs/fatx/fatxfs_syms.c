/*
 *  linux/fs/fatx/fatxfs_syms.c
 *
 *  Exported kernel symbols for the FATX filesystem.
 *
 *  Written 2003 by Edgar Hucek and Lehner Franz
 *
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/fatx_fs.h>
#include <linux/init.h>

unsigned int fatx_debug = 0;

MODULE_PARM(fatx_debug,"i");
MODULE_PARM_DESC(fatx_debug,"turn on fatx debugging output");

EXPORT_SYMBOL(fatx_lookup);
EXPORT_SYMBOL(fatx_create);
EXPORT_SYMBOL(fatx_rmdir);
EXPORT_SYMBOL(fatx_mkdir);
EXPORT_SYMBOL(fatx_rename);
EXPORT_SYMBOL(fatx_unlink);

EXPORT_SYMBOL(fatx_new_dir);
EXPORT_SYMBOL(fatx_get_block);
EXPORT_SYMBOL(fatx_clear_inode);
EXPORT_SYMBOL(fatx_date_unix2dos);
EXPORT_SYMBOL(fatx_delete_inode);
EXPORT_SYMBOL(fatx_get_entry);
EXPORT_SYMBOL(fatx_notify_change);
EXPORT_SYMBOL(fatx_put_super);
EXPORT_SYMBOL(fatx_attach);
EXPORT_SYMBOL(fatx_detach);
EXPORT_SYMBOL(fatx_build_inode);
EXPORT_SYMBOL(fatx_fill_super);
EXPORT_SYMBOL(fatx_readdir);
EXPORT_SYMBOL(fatx_scan);
EXPORT_SYMBOL(fatx_statfs);
EXPORT_SYMBOL(fatx_write_inode);
EXPORT_SYMBOL(fatx_get_cluster);
EXPORT_SYMBOL(fatx_add_entries);
EXPORT_SYMBOL(fatx_dir_empty);
EXPORT_SYMBOL(fatx_truncate);

static struct super_block *fatx_get_sb(struct file_system_type *fs_type,
		        int flags, const char *dev_name, void *data)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, fatx_fill_super);
}

static struct file_system_type fatx_fs_type = {
	.owner          = THIS_MODULE,
	.name           = "fatx",
	.get_sb         = fatx_get_sb,
	.kill_sb        = kill_block_super,
	.fs_flags       = FS_REQUIRES_DEV,
};

static int __init init_fatx_fs(void)
{
	printk("FATX driver Version 0.1.0\n");
	fatx_hash_init();
	return register_filesystem(&fatx_fs_type);
}

static void __exit exit_fatx_fs(void)
{
	unregister_filesystem(&fatx_fs_type);
}

module_init(init_fatx_fs)
module_exit(exit_fatx_fs)
MODULE_LICENSE("GPL");

