/* super.c - Enhanced with forced disk persistence */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/blkdev.h>

#include "simplefs.h"

static struct kmem_cache *simplefs_inode_cache;

/* Force write buffer to disk immediately */
static int force_write_buffer(struct buffer_head *bh)
{
    int ret;
    
    mark_buffer_dirty(bh);
    ret = sync_dirty_buffer(bh);
    if (ret) {
        pr_err("Failed to sync buffer to disk: %d\n", ret);
        return ret;
    }
    
    /* Additional barrier to ensure write completion */
    if (bh->b_bdev && bh->b_bdev->bd_disk) {
        ret = blkdev_issue_flush(bh->b_bdev, GFP_KERNEL, NULL);
        if (ret) {
            pr_warn("Failed to flush device: %d\n", ret);
        }
    }
    
    return 0;
}

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
        /* Final sync before unmount */
        pr_info("Final superblock sync before unmount\n");
        simplefs_sync_sb(sb);
        kfree(sbi);
    }
}

static int simplefs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
    /* For now, we don't implement individual inode writing */
    /* All inode updates are done immediately in operations */
    return 0;
}

/* Sync superblock changes to disk with forced write */
int simplefs_sync_sb(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;
    struct simplefs_sb_info *disk_sb;
    int ret;

    pr_info("Syncing superblock to disk: free_blocks=%u, free_inodes=%u\n",
            sbi->nr_free_blocks, sbi->nr_free_inodes);

    bh = sb_bread(sb, SIMPLEFS_SUPERBLOCK_BLOCK);
    if (!bh) {
        pr_err("Failed to read superblock for sync\n");
        return -EIO;
    }

    disk_sb = (struct simplefs_sb_info *)bh->b_data;
    
    /* Update on-disk superblock with current values */
    disk_sb->nr_free_blocks = cpu_to_le32(sbi->nr_free_blocks);
    disk_sb->nr_free_inodes = cpu_to_le32(sbi->nr_free_inodes);
    
    /* Force immediate write to disk */
    ret = force_write_buffer(bh);
    brelse(bh);
    
    if (ret == 0) {
        pr_info("Superblock synced to disk successfully: free_blocks=%u, free_inodes=%u\n",
                sbi->nr_free_blocks, sbi->nr_free_inodes);
    } else {
        pr_err("Failed to sync superblock to disk: %d\n", ret);
    }
    
    return ret;
}

static int simplefs_statfs(struct dentry *dentry, struct kstatfs *stat)
{
    struct super_block *sb = dentry->d_sb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);

    /* Re-read superblock from disk to get current state */
    struct buffer_head *bh = sb_bread(sb, SIMPLEFS_SUPERBLOCK_BLOCK);
    if (bh) {
        struct simplefs_sb_info *disk_sb = (struct simplefs_sb_info *)bh->b_data;
        
        /* Update in-memory values from disk (in case of inconsistency) */
        sbi->nr_free_blocks = le32_to_cpu(disk_sb->nr_free_blocks);
        sbi->nr_free_inodes = le32_to_cpu(disk_sb->nr_free_inodes);
        
        brelse(bh);
        
        pr_info("statfs: Updated from disk - free_blocks=%u, free_inodes=%u\n",
                sbi->nr_free_blocks, sbi->nr_free_inodes);
    }

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

static int simplefs_sync_fs(struct super_block *sb, int wait)
{
    pr_info("sync_fs called (wait=%d)\n", wait);
    return simplefs_sync_sb(sb);
}

static const struct super_operations simplefs_super_ops = {
    .alloc_inode = simplefs_alloc_inode,
    .destroy_inode = simplefs_destroy_inode,
    .put_super = simplefs_put_super,
    .write_inode = simplefs_write_inode,
    .statfs = simplefs_statfs,
    .sync_fs = simplefs_sync_fs,
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
    sbi->magic = le32_to_cpu(csb->magic);
    sbi->nr_blocks = le32_to_cpu(csb->nr_blocks);
    sbi->nr_inodes = le32_to_cpu(csb->nr_inodes);
    sbi->nr_free_blocks = le32_to_cpu(csb->nr_free_blocks);
    sbi->nr_free_inodes = le32_to_cpu(csb->nr_free_inodes);
    sbi->inode_bitmap_block = le32_to_cpu(csb->inode_bitmap_block);
    sbi->block_bitmap_block = le32_to_cpu(csb->block_bitmap_block);
    sbi->first_data_block = le32_to_cpu(csb->first_data_block);
    
    sb->s_fs_info = sbi;
    brelse(bh);

    pr_info("Mounted filesystem from disk: %u blocks, %u inodes\n", 
            sbi->nr_blocks, sbi->nr_inodes);
    pr_info("Current disk state - Free: %u blocks, %u inodes\n", 
            sbi->nr_free_blocks, sbi->nr_free_inodes);
    pr_info("Bitmaps: inode=%u, block=%u, first_data=%u\n",
            sbi->inode_bitmap_block, sbi->block_bitmap_block, sbi->first_data_block);

    /* Validate superblock values */
    if (sbi->nr_blocks == 0 || sbi->nr_inodes == 0) {
        pr_err("Invalid superblock: nr_blocks=%u, nr_inodes=%u\n",
               sbi->nr_blocks, sbi->nr_inodes);
        ret = -EINVAL;
        goto free_sbi;
    }

    if (sbi->first_data_block >= sbi->nr_blocks) {
        pr_err("Invalid first_data_block=%u (nr_blocks=%u)\n",
               sbi->first_data_block, sbi->nr_blocks);
        ret = -EINVAL;
        goto free_sbi;
    }

    /* Additional consistency check: verify bitmap blocks exist */
    if (sbi->inode_bitmap_block >= sbi->nr_blocks || 
        sbi->block_bitmap_block >= sbi->nr_blocks) {
        pr_err("Invalid bitmap blocks: inode_bitmap=%u, block_bitmap=%u (nr_blocks=%u)\n",
               sbi->inode_bitmap_block, sbi->block_bitmap_block, sbi->nr_blocks);
        ret = -EINVAL;
        goto free_sbi;
    }

    /* Get root inode (inode 1) */
    pr_info("Loading root inode (inode 1)\n");
    root_inode = simplefs_iget(sb, 1);
    if (IS_ERR(root_inode)) {
        pr_err("Failed to get root inode: %ld\n", PTR_ERR(root_inode));
        ret = PTR_ERR(root_inode);
        goto free_sbi;
    }
    
    pr_info("Root inode loaded successfully from disk\n");
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

    pr_info("Superblock setup complete - filesystem ready with disk persistence\n");
    return 0;

iput:
    iput(root_inode);
free_sbi:
    kfree(sbi);
release:
    brelse(bh);
    return ret;
}