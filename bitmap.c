/* bitmap.c */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

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

static int find_first_zero_bit_in_buffer(unsigned char *buffer, int max_bits)
{
    int i;
    for (i = 0; i < max_bits; i++) {
        if (!test_bit_in_buffer(buffer, i)) {
            return i;
        }
    }
    return -1;  /* No free bit found */
}

/* Allocate a free inode number */
int simplefs_alloc_inode_num(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;
    unsigned char *bitmap;
    int free_inode;

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
    
    /* Find first free inode (skip inode 0 which is reserved) */
    free_inode = find_first_zero_bit_in_buffer(bitmap, sbi->nr_inodes);
    if (free_inode <= 0 || free_inode >= sbi->nr_inodes) {
        brelse(bh);
        pr_err("No free inode found in bitmap\n");
        return -ENOSPC;
    }

    /* Mark inode as used */
    set_bit_in_buffer(bitmap, free_inode);
    
    /* Mark buffer as dirty and release */
    mark_buffer_dirty(bh);
    brelse(bh);

    /* Update superblock free count */
    sbi->nr_free_inodes--;

    pr_info("Allocated inode %d\n", free_inode);
    return free_inode;
}

/* Free an inode number */
void simplefs_free_inode_num(struct super_block *sb, unsigned long ino)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;
    unsigned char *bitmap;

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
    
    /* Mark buffer as dirty and release */
    mark_buffer_dirty(bh);
    brelse(bh);

    /* Update superblock free count */
    sbi->nr_free_inodes++;

    pr_info("Freed inode %lu\n", ino);
}

/* Allocate a free block number */
int simplefs_alloc_block_num(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;
    unsigned char *bitmap;
    int free_block;

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
    free_block = find_first_zero_bit_in_buffer(bitmap, sbi->nr_blocks);
    if (free_block < 0 || free_block >= sbi->nr_blocks) {
        brelse(bh);
        pr_err("No free block found in bitmap\n");
        return -ENOSPC;
    }

    /* Make sure we don't allocate reserved blocks */
    if (free_block < sbi->first_data_block) {
        /* Find first free block after reserved area */
        int i;
        for (i = sbi->first_data_block; i < sbi->nr_blocks; i++) {
            if (!test_bit_in_buffer(bitmap, i)) {
                free_block = i;
                break;
            }
        }
        if (i >= sbi->nr_blocks) {
            brelse(bh);
            pr_err("No free data block found\n");
            return -ENOSPC;
        }
    }

    /* Mark block as used */
    set_bit_in_buffer(bitmap, free_block);
    
    /* Mark buffer as dirty and release */
    mark_buffer_dirty(bh);
    brelse(bh);

    /* Update superblock free count */
    sbi->nr_free_blocks--;

    pr_info("Allocated block %d\n", free_block);
    return free_block;
}

/* Free a block number */
void simplefs_free_block_num(struct super_block *sb, unsigned long block)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;
    unsigned char *bitmap;

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
    
    /* Mark buffer as dirty and release */
    mark_buffer_dirty(bh);
    brelse(bh);

    /* Update superblock free count */
    sbi->nr_free_blocks++;

    pr_info("Freed block %lu\n", block);
}