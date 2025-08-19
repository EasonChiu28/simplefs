/* super.c */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "simplefs.h"

static struct kmem_cache *simplefs_inode_cache;

int simplefs_init_inode_cache(void)
{
    simplefs_inode_cache = kmem_cache_create("simplefs_cache",
                                             sizeof(struct simplefs_inode_info),
                                             0, 0, NULL);
    if (!simplefs_inode_cache)
        return -ENOMEM;
    return 0;
}

void simplefs_destroy_inode_cache(void)
{
    kmem_cache_destroy(simplefs_inode_cache);
}

static struct inode *simplefs_alloc_inode(struct super_block *sb)
{
    struct simplefs_inode_info *ci =
        kmem_cache_alloc(simplefs_inode_cache, GFP_KERNEL);
    if (!ci)
        return NULL;
    return &ci->vfs_inode;
}

static void simplefs_destroy_inode(struct inode *inode)
{
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    kmem_cache_free(simplefs_inode_cache, ci);
}

static void simplefs_put_super(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    if (sbi) {
        kfree(sbi);
    }
}

static int simplefs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
    return 0;
}

static int simplefs_statfs(struct dentry *dentry, struct kstatfs *stat)
{
    struct super_block *sb = dentry->d_sb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);

    stat->f_type = SIMPLEFS_MAGIC;
    stat->f_bsize = SIMPLEFS_BLOCK_SIZE;
    stat->f_blocks = sbi->nr_blocks;
    stat->f_bfree = sbi->nr_free_blocks;
    stat->f_bavail = sbi->nr_free_blocks;
    stat->f_files = sbi->nr_inodes;
    stat->f_ffree = sbi->nr_free_inodes;
    stat->f_namelen = SIMPLEFS_FILENAME_LEN;

    return 0;
}

static const struct super_operations simplefs_super_ops = {
    .alloc_inode = simplefs_alloc_inode,
    .destroy_inode = simplefs_destroy_inode,
    .put_super = simplefs_put_super,
    .write_inode = simplefs_write_inode,
    .statfs = simplefs_statfs,
};

int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct buffer_head *bh;
    struct simplefs_sb_info *csb;
    struct simplefs_sb_info *sbi;
    struct inode *root_inode;
    int ret = 0;

    pr_info("Starting superblock setup\n");

    sb->s_magic = SIMPLEFS_MAGIC;
    sb_set_blocksize(sb, SIMPLEFS_BLOCK_SIZE);
    sb->s_op = &simplefs_super_ops;

    /* Read superblock from disk */
    bh = sb_bread(sb, SIMPLEFS_SUPERBLOCK_BLOCK);
    if (!bh) {
        pr_err("Failed to read superblock\n");
        return -EIO;
    }
    csb = (struct simplefs_sb_info *) bh->b_data;

    /* Validate magic number */
    if (le32_to_cpu(csb->magic) != SIMPLEFS_MAGIC) {
        pr_err("Wrong magic number: expected 0x%x, got 0x%x\n", 
               SIMPLEFS_MAGIC, le32_to_cpu(csb->magic));
        ret = -EINVAL;
        goto release;
    }

    /* Allocate in-memory superblock info */
    sbi = kzalloc(sizeof(struct simplefs_sb_info), GFP_KERNEL);
    if (!sbi) {
        ret = -ENOMEM;
        goto release;
    }

    /* Copy superblock data to in-memory structure */
    sbi->nr_blocks = le32_to_cpu(csb->nr_blocks);
    sbi->nr_inodes = le32_to_cpu(csb->nr_inodes);
    sbi->nr_free_blocks = le32_to_cpu(csb->nr_free_blocks);
    sbi->nr_free_inodes = le32_to_cpu(csb->nr_free_inodes);
    sbi->inode_bitmap_block = le32_to_cpu(csb->inode_bitmap_block);
    sbi->block_bitmap_block = le32_to_cpu(csb->block_bitmap_block);
    sbi->first_data_block = le32_to_cpu(csb->first_data_block);
    
    sb->s_fs_info = sbi;
    brelse(bh);

    pr_info("Mounted filesystem: %u blocks, %u inodes\n", 
            sbi->nr_blocks, sbi->nr_inodes);
    pr_info("Free: %u blocks, %u inodes\n", 
            sbi->nr_free_blocks, sbi->nr_free_inodes);
    pr_info("Bitmaps: inode=%u, block=%u, first_data=%u\n",
            sbi->inode_bitmap_block, sbi->block_bitmap_block, sbi->first_data_block);

    /* Get root inode (inode 1) */
    pr_info("Loading root inode (inode 1)\n");
    root_inode = simplefs_iget(sb, 1);
    if (IS_ERR(root_inode)) {
        pr_err("Failed to get root inode: %ld\n", PTR_ERR(root_inode));
        ret = PTR_ERR(root_inode);
        goto free_sbi;
    }
    
    pr_info("Root inode loaded successfully\n");
    pr_info("Root inode: mode=0x%x, size=%lld, i_fop=%p\n", 
            root_inode->i_mode, root_inode->i_size, root_inode->i_fop);

    /* Create root dentry */
    pr_info("Creating root dentry\n");
    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        pr_err("Failed to create root dentry\n");
        ret = -ENOMEM;
        goto iput;
    }

    pr_info("Superblock setup complete\n");
    return 0;

iput:
    iput(root_inode);
free_sbi:
    kfree(sbi);
release:
    brelse(bh);
    return ret;
}