/* dir.c */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "simplefs.h"

/*
 * Iterate over the files contained in dir and commit them in ctx.
 * This function is called by the VFS while ctx->pos changes.
 * Return 0 on success.
 */
static int simplefs_iterate(struct file *dir, struct dir_context *ctx)
{
    struct inode *inode = file_inode(dir);
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh = NULL;
    struct simplefs_dir_block *dblock = NULL;
    struct simplefs_file *f = NULL;
    int i;

    pr_info("Directory iteration: pos=%lld, inode=%lu, block=%u\n", 
            ctx->pos, inode->i_ino, ci->ei_block);

    /* Check that dir is a directory */
    if (!S_ISDIR(inode->i_mode))
        return -ENOTDIR;

    /*
     * Check that ctx->pos is not bigger than what we can handle (including
     * . and ..)
     */
    if (ctx->pos > SIMPLEFS_MAX_SUBFILES + 2)
        return 0;

    /* Commit . and .. to ctx - this handles both entries at once */
    if (!dir_emit_dots(dir, ctx))
        return 0;

    /* Read the directory index block on disk */
    bh = sb_bread(sb, ci->ei_block);
    if (!bh) {
        pr_err("Failed to read directory block %u\n", ci->ei_block);
        return -EIO;
    }
    dblock = (struct simplefs_dir_block *) bh->b_data;

    pr_info("Directory loaded, nr_files=%u, starting from file index %d\n", 
            le32_to_cpu(dblock->nr_files), (int)(ctx->pos - 2));

    /* Iterate over the index block and commit subfiles */
    for (i = ctx->pos - 2; i < SIMPLEFS_MAX_SUBFILES; i++) {
        f = &dblock->files[i];
        
        /* If inode is 0, we've reached the end of files */
        if (!f->inode) {
            pr_info("End of directory entries at index %d\n", i);
            break;
        }
        
        pr_info("Emitting file[%d]: '%s' -> inode %u\n", i, f->filename, le32_to_cpu(f->inode));
        
        /* Emit the directory entry */
        if (!dir_emit(ctx, f->filename, strnlen(f->filename, SIMPLEFS_FILENAME_LEN), 
                      le32_to_cpu(f->inode), DT_UNKNOWN)) {
            pr_info("dir_emit failed for '%s'\n", f->filename);
            break;
        }
        
        pr_info("Successfully emitted '%s'\n", f->filename);
        ctx->pos++;
    }

    brelse(bh);
    pr_info("Directory iteration complete, final pos=%lld\n", ctx->pos);

    return 0;
}

const struct file_operations simplefs_dir_ops = {
    .owner = THIS_MODULE,
    .iterate_shared = simplefs_iterate,
};