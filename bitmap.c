/* bitmap.c - Enhanced with forced disk persistence */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/blkdev.h>

#include "simplefs.h"

/* Helper functions for bitmap operations */
static void set_bit_in_buffer(unsigned char *buffer, int bit)
{
    buffer[bit / 8] |= (1 << (bit % 8));
}

static void clear_bit_in_buffer(unsigned char *buffer, int bit)
{
    buffer[bit / 8] &= ~(1 << (bit % 8));
}

static int test_bit_in_buffer(unsigned char *buffer, int bit)
{
    return (buffer[bit / 8] & (1 << (bit % 8))) != 0;
}

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

/* Update superblock on disk with forced write */
static int simplefs_update_sb(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;
    struct simplefs_sb_info *disk_sb;
    int ret;

    pr_info("Updating superblock: free_blocks=%u, free_inodes=%u\n",
            sbi->nr_free_blocks, sbi->nr_free_inodes);

    bh = sb_bread(sb, SIMPLEFS_SUPERBLOCK_BLOCK);
    if (!bh) {
        pr_err("Failed to read superblock for update\n");
        return -EIO;
    }

    disk_sb = (struct simplefs_sb_info *)bh->b_data;
    
    /* Update free counts on disk */
    disk_sb->nr_free_blocks = cpu_to_le32(sbi->nr_free_blocks);
    disk_sb->nr_free_inodes = cpu_to_le32(sbi->nr_free_inodes);
    
    /* Force immediate write to disk */
    ret = force_write_buffer(bh);
    brelse(bh);
    
    if (ret == 0) {
        pr_info("Superblock updated on disk: free_blocks=%u, free_inodes=%u\n",
                sbi->nr_free_blocks, sbi->nr_free_inodes);
    }
    
    return ret;
}

/* Allocate a free inode number */
int simplefs_alloc_inode_num(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;
    unsigned char *bitmap;
    int free_inode;
    int i, ret;

    pr_info("Allocating inode: free_inodes=%u, total_inodes=%u\n", 
            sbi->nr_free_inodes, sbi->nr_inodes);

    if (sbi->nr_free_inodes == 0) {
        pr_err("No free inodes available\n");
        return -ENOSPC;
    }

    /* Read inode bitmap */
    bh = sb_bread(sb, sbi->inode_bitmap_block);
    if (!bh) {
        pr_err("Failed to read inode bitmap block %u\n", sbi->inode_bitmap_block);
        return -EIO;
    }

    bitmap = (unsigned char *)bh->b_data;
    
    /* Find first free inode starting from inode 1 (skip inode 0 which is reserved) */
    free_inode = -1;
    for (i = 1; i < sbi->nr_inodes; i++) {
        if (!test_bit_in_buffer(bitmap, i)) {
            free_inode = i;
            break;
        }
    }
    
    if (free_inode < 0) {
        brelse(bh);
        pr_err("No free inode found in bitmap (searched %u inodes)\n", sbi->nr_inodes);
        /* Debug: show first few bytes of bitmap */
        bh = sb_bread(sb, sbi->inode_bitmap_block);
        if (bh) {
            pr_err("Bitmap first 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   bh->b_data[0], bh->b_data[1], bh->b_data[2], bh->b_data[3],
                   bh->b_data[4], bh->b_data[5], bh->b_data[6], bh->b_data[7]);
            brelse(bh);
        }
        return -ENOSPC;
    }

    /* Mark inode as used */
    set_bit_in_buffer(bitmap, free_inode);
    
    /* Force write bitmap to disk immediately */
    ret = force_write_buffer(bh);
    brelse(bh);
    
    if (ret) {
        pr_err("Failed to write inode bitmap to disk: %d\n", ret);
        return ret;
    }

    /* Update superblock free count and write to disk */
    sbi->nr_free_inodes--;
    ret = simplefs_update_sb(sb);
    if (ret) {
        /* Rollback: mark inode as free again */
        bh = sb_bread(sb, sbi->inode_bitmap_block);
        if (bh) {
            clear_bit_in_buffer((unsigned char *)bh->b_data, free_inode);
            force_write_buffer(bh);
            brelse(bh);
        }
        sbi->nr_free_inodes++;
        return ret;
    }

    pr_info("Allocated inode %d (remaining: %u) - written to disk\n", 
            free_inode, sbi->nr_free_inodes);
    return free_inode;
}

/* Free an inode number */
void simplefs_free_inode_num(struct super_block *sb, unsigned long ino)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;
    unsigned char *bitmap;
    int ret;

    pr_info("Freeing inode %lu\n", ino);

    if (ino == 0 || ino >= sbi->nr_inodes) {
        pr_err("Invalid inode number %lu\n", ino);
        return;
    }

    /* Read inode bitmap */
    bh = sb_bread(sb, sbi->inode_bitmap_block);
    if (!bh) {
        pr_err("Failed to read inode bitmap block %u\n", sbi->inode_bitmap_block);
        return;
    }

    bitmap = (unsigned char *)bh->b_data;
    
    /* Check if inode is actually allocated */
    if (!test_bit_in_buffer(bitmap, ino)) {
        pr_warn("Trying to free already free inode %lu\n", ino);
        brelse(bh);
        return;
    }

    /* Mark inode as free */
    clear_bit_in_buffer(bitmap, ino);
    
    /* Force write bitmap to disk immediately */
    ret = force_write_buffer(bh);
    brelse(bh);
    
    if (ret) {
        pr_err("Failed to write inode bitmap to disk: %d\n", ret);
        return;
    }

    /* Update superblock free count and write to disk */
    sbi->nr_free_inodes++;
    ret = simplefs_update_sb(sb);
    if (ret) {
        pr_err("Failed to update superblock after freeing inode\n");
        /* Note: We could rollback here, but it's complex and this is just a warning */
    }

    pr_info("Freed inode %lu - written to disk (remaining free: %u)\n", 
            ino, sbi->nr_free_inodes);
}

/* Allocate a free block number */
int simplefs_alloc_block_num(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;
    unsigned char *bitmap;
    int free_block;
    int i, ret;

    pr_info("Allocating block: free_blocks=%u, first_data=%u\n", 
            sbi->nr_free_blocks, sbi->first_data_block);

    if (sbi->nr_free_blocks == 0) {
        pr_err("No free blocks available\n");
        return -ENOSPC;
    }

    /* Read block bitmap */
    bh = sb_bread(sb, sbi->block_bitmap_block);
    if (!bh) {
        pr_err("Failed to read block bitmap block %u\n", sbi->block_bitmap_block);
        return -EIO;
    }

    bitmap = (unsigned char *)bh->b_data;
    
    /* Find first free block starting from first data block */
    free_block = -1;
    for (i = sbi->first_data_block; i < sbi->nr_blocks; i++) {
        if (!test_bit_in_buffer(bitmap, i)) {
            free_block = i;
            break;
        }
    }
    
    if (free_block < 0) {
        brelse(bh);
        pr_err("No free data block found\n");
        return -ENOSPC;
    }

    /* Mark block as used */
    set_bit_in_buffer(bitmap, free_block);
    
    /* Force write bitmap to disk immediately */
    ret = force_write_buffer(bh);
    brelse(bh);
    
    if (ret) {
        pr_err("Failed to write block bitmap to disk: %d\n", ret);
        return ret;
    }

    /* Update superblock free count and write to disk */
    sbi->nr_free_blocks--;
    ret = simplefs_update_sb(sb);
    if (ret) {
        /* Rollback: mark block as free again */
        bh = sb_bread(sb, sbi->block_bitmap_block);
        if (bh) {
            clear_bit_in_buffer((unsigned char *)bh->b_data, free_block);
            force_write_buffer(bh);
            brelse(bh);
        }
        sbi->nr_free_blocks++;
        return ret;
    }

    pr_info("Allocated block %d (remaining: %u) - written to disk\n", 
            free_block, sbi->nr_free_blocks);
    return free_block;
}

/* Free a block number */
void simplefs_free_block_num(struct super_block *sb, unsigned long block)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;
    unsigned char *bitmap;
    int ret;

    pr_info("Freeing block %lu\n", block);

    if (block < sbi->first_data_block || block >= sbi->nr_blocks) {
        pr_err("Invalid block number %lu\n", block);
        return;
    }

    /* Read block bitmap */
    bh = sb_bread(sb, sbi->block_bitmap_block);
    if (!bh) {
        pr_err("Failed to read block bitmap block %u\n", sbi->block_bitmap_block);
        return;
    }

    bitmap = (unsigned char *)bh->b_data;
    
    /* Check if block is actually allocated */
    if (!test_bit_in_buffer(bitmap, block)) {
        pr_warn("Trying to free already free block %lu\n", block);
        brelse(bh);
        return;
    }

    /* Mark block as free */
    clear_bit_in_buffer(bitmap, block);
    
    /* Force write bitmap to disk immediately */
    ret = force_write_buffer(bh);
    brelse(bh);
    
    if (ret) {
        pr_err("Failed to write block bitmap to disk: %d\n", ret);
        return;
    }

    /* Update superblock free count and write to disk */
    sbi->nr_free_blocks++;
    ret = simplefs_update_sb(sb);
    if (ret) {
        pr_err("Failed to update superblock after freeing block\n");
        /* Note: We could rollback here, but it's complex and this is just a warning */
    }

    pr_info("Freed block %lu - written to disk (remaining free: %u)\n", 
            block, sbi->nr_free_blocks);
}