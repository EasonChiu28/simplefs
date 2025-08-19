/* fs.c */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "simplefs.h"

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("a simple file system");

struct dentry *simplefs_mount(struct file_system_type *fs_type,
                              int flags,
                              const char *dev_name,
                              void *data)
{
    struct dentry *dentry =
        mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);

    if (IS_ERR(dentry))
        pr_err("'%s' mount failure\n", dev_name);
    else
        pr_info("'%s' mount success\n", dev_name);

    return dentry;
}

void simplefs_kill_sb(struct super_block *sb)
{
    kill_block_super(sb);
    pr_info("unmounted disk\n");
}

static struct file_system_type simplefs_file_system_type = {
    .owner = THIS_MODULE,
    .name = "simplefs",
    .mount = simplefs_mount,
    .kill_sb = simplefs_kill_sb,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init simplefs_init(void)
{
    int ret;
    
    ret = simplefs_init_inode_cache();
    if (ret) {
        pr_err("Failed to create inode cache\n");
        return ret;
    }

    ret = register_filesystem(&simplefs_file_system_type);
    if (ret) {
        pr_err("Failed to register file system\n");
        simplefs_destroy_inode_cache();
        return ret;
    }

    pr_info("module loaded\n");
    return 0;
}

static void __exit simplefs_exit(void)
{
    int ret;
    
    ret = unregister_filesystem(&simplefs_file_system_type);
    if (ret)
        pr_err("Failed to unregister file system\n");

    simplefs_destroy_inode_cache();

    pr_info("module unloaded\n");
}

module_init(simplefs_init);
module_exit(simplefs_exit);
