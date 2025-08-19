/* dir.c */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "simplefs.h"

static int simplefs_iterate(struct file *dir, struct dir_context *ctx)
{
    struct inode *inode = file_inode(dir);
    struct super_block *sb = inode->i_sb;
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct buffer_head *bh;
    struct simplefs_dir_block *dblock;
    uint32_t nr_files;
    int i, start_index;

    /* Handle . and .. entries first */
    if (!dir_emit_dots(dir, ctx)) {
        return 0;
    }

    /* Check if we're past all possible entries */
    if (ctx->pos >= SIMPLEFS_MAX_SUBFILES + 2) {
        return 0;
    }

    /* Read directory block */
    bh = sb_bread(sb, ci->ei_block);
    if (!bh) {
        return -EIO;
    }

    dblock = (struct simplefs_dir_block *)bh->b_data;
    nr_files = le32_to_cpu(dblock->nr_files);
    
    /* Bounds checking */
    if (nr_files > SIMPLEFS_MAX_SUBFILES) {
        brelse(bh);
        return -EIO;
    }

    /* Calculate starting index */
    start_index = ctx->pos - 2;
    
    /* Iterate through files */
    for (i = start_index; i < nr_files; i++) {
        struct simplefs_file *f = &dblock->files[i];
        uint32_t ino;
        
        if (i < 0) continue;
        
        ino = le32_to_cpu(f->inode);
        
        /* Validate inode number */
        if (ino == 0 || ino > SIMPLEFS_MAX_SUBFILES) {
            ctx->pos++;
            continue;
        }
        
        /* Ensure null termination */
        f->filename[SIMPLEFS_FILENAME_LEN - 1] = '\0';
        
        /* Skip empty names */
        if (f->filename[0] == '\0') {
            ctx->pos++;
            continue;
        }
        
        /* Emit the entry */
        if (!dir_emit(ctx, f->filename, strlen(f->filename), ino, DT_REG)) {
            brelse(bh);
            return 0;
        }
        
        ctx->pos++;
    }

    brelse(bh);
    return 0;
}

const struct file_operations simplefs_dir_ops = {
    .owner = THIS_MODULE,
    .iterate_shared = simplefs_iterate,
    .llseek = generic_file_llseek,
};