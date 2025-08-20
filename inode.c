/* inode.c - Simple mkdir support (step by step approach) */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pagemap.h>

#include "simplefs.h"

static struct dentry *simplefs_lookup(struct inode *dir,
                                      struct dentry *dentry,
                                      unsigned int flags);
static int simplefs_readpage(struct file *file, struct page *page);

/* Let's start with just the basic operations - same as original */
static const struct inode_operations simplefs_inode_ops = {
    .lookup = simplefs_lookup,
};

static const struct file_operations simplefs_file_ops = {
    .owner = THIS_MODULE,
    .read_iter = generic_file_read_iter,
    .llseek = generic_file_llseek,
};

static const struct address_space_operations simplefs_aops = {
    .readpage = simplefs_readpage,
};

struct inode *simplefs_iget(struct super_block *sb, unsigned long ino)
{
    struct inode *inode;
    struct simplefs_inode *raw_inode;
    struct simplefs_inode_info *ci;
    struct buffer_head *bh;
    uint32_t inode_block = (ino / SIMPLEFS_INODES_PER_BLOCK) + 1;
    uint32_t inode_shift = ino % SIMPLEFS_INODES_PER_BLOCK;

    pr_info("Loading inode %lu (block %u, shift %u)\n", ino, inode_block, inode_shift);

    inode = iget_locked(sb, ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW))
        return inode;

    ci = SIMPLEFS_INODE(inode);

    bh = sb_bread(sb, inode_block);
    if (!bh) {
        pr_err("Failed to read inode block %u\n", inode_block);
        iget_failed(inode);
        return ERR_PTR(-EIO);
    }
    raw_inode = (struct simplefs_inode *)bh->b_data;
    raw_inode += inode_shift;

    inode->i_mode = le32_to_cpu(raw_inode->i_mode);
    i_uid_write(inode, le32_to_cpu(raw_inode->i_uid));
    i_gid_write(inode, le32_to_cpu(raw_inode->i_gid));
    inode->i_size = le32_to_cpu(raw_inode->i_size);
    set_nlink(inode, le32_to_cpu(raw_inode->i_nlink));
    ci->ei_block = le32_to_cpu(raw_inode->ei_block);

    /* Set timestamps to prevent issues */
    inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

    /* Initialize additional inode fields that might be expected */
    inode->i_blocks = 1;
    inode->i_blkbits = sb->s_blocksize_bits;
    
    /* For simplicity, disable atime updates to avoid I/O list complications */
    inode->i_flags |= S_NOATIME;

    pr_info("Inode %lu: mode=0x%x, size=%lld, block=%u\n", 
             ino, inode->i_mode, inode->i_size, ci->ei_block);

    if (S_ISDIR(inode->i_mode)) {
        pr_info("Setting up directory inode %lu\n", ino);
        inode->i_op = &simplefs_inode_ops;
        inode->i_fop = &simplefs_dir_ops;
        /* Important: set directory size properly */
        inode->i_size = SIMPLEFS_BLOCK_SIZE;
        pr_info("Directory inode %lu: block=%u, fop=%p\n", ino, ci->ei_block, inode->i_fop);
    } else if (S_ISREG(inode->i_mode)) {
        pr_info("Setting up regular file inode %lu\n", ino);
        inode->i_op = &simplefs_inode_ops;
        inode->i_fop = &simplefs_file_ops;
        inode->i_mapping->a_ops = &simplefs_aops;
        pr_info("File inode %lu: block=%u, size=%lld\n", ino, ci->ei_block, inode->i_size);
    } else {
        pr_err("Unknown inode type: mode=0x%x for inode %lu\n", inode->i_mode, ino);
        brelse(bh);
        iget_failed(inode);
        return ERR_PTR(-EINVAL);
    }

    brelse(bh);
    unlock_new_inode(inode);
    pr_info("Successfully loaded inode %lu\n", ino);
    return inode;
}

static struct dentry *simplefs_lookup(struct inode *dir,
                                      struct dentry *dentry,
                                      unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct simplefs_inode_info *ci_dir = SIMPLEFS_INODE(dir);
    struct buffer_head *bh;
    struct simplefs_dir_block *dblock;
    struct inode *inode = NULL;
    uint32_t nr_files;
    int i;

    pr_info("Looking up '%s' in directory inode %lu (block %u)\n", 
             dentry->d_name.name, dir->i_ino, ci_dir->ei_block);

    bh = sb_bread(sb, ci_dir->ei_block);
    if (!bh) {
        pr_err("Failed to read directory block %u\n", ci_dir->ei_block);
        return ERR_PTR(-EIO);
    }

    dblock = (struct simplefs_dir_block *)bh->b_data;
    nr_files = le32_to_cpu(dblock->nr_files);

    pr_info("Directory has %u files\n", nr_files);

    if (nr_files > SIMPLEFS_MAX_SUBFILES) {
        pr_err("Invalid nr_files %u\n", nr_files);
        brelse(bh);
        return ERR_PTR(-EIO);
    }

    for (i = 0; i < nr_files && i < SIMPLEFS_MAX_SUBFILES; i++) {
        struct simplefs_file *f = &dblock->files[i];
        uint32_t ino = le32_to_cpu(f->inode);
        
        /* Ensure null termination */
        f->filename[SIMPLEFS_FILENAME_LEN - 1] = '\0';
        
        pr_info("Checking file[%d]: '%s' -> inode %u\n", i, f->filename, ino);
        
        if (strcmp(dentry->d_name.name, f->filename) == 0) {
            pr_info("Found match: '%s' -> inode %u\n", f->filename, ino);
            if (ino > 0 && ino <= SIMPLEFS_MAX_SUBFILES) {
                inode = simplefs_iget(sb, ino);
                if (IS_ERR(inode)) {
                    pr_err("Failed to load inode %u: %ld\n", ino, PTR_ERR(inode));
                    inode = NULL;
                }
            }
            break;
        }
    }
    brelse(bh);

    pr_info("Lookup complete: '%s' -> %s\n", dentry->d_name.name, 
            inode ? "found" : "not found");
    return d_splice_alias(inode, dentry);
}

static int simplefs_readpage(struct file *file, struct page *page)
{
    struct inode *inode = file_inode(file);
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct buffer_head *bh;
    void *kaddr;
    size_t copy_size;

    /* Only handle first page for simple filesystem */
    if (page->index > 0) {
        /* Beyond file scope, zero the page */
        kaddr = kmap(page);
        memset(kaddr, 0, PAGE_SIZE);
        kunmap(page);
        SetPageUptodate(page);
        unlock_page(page);
        return 0;
    }

    /* Map the page */
    kaddr = kmap(page);
    
    /* If empty file or no data block, zero the page */
    if (inode->i_size == 0 || ci->ei_block == 0) {
        memset(kaddr, 0, PAGE_SIZE);
        goto done;
    }

    /* Validate block number - ensure it's a reasonable data block */
    if (ci->ei_block < 4 || ci->ei_block >= 12800) {
        pr_err("Invalid data block %u for inode %lu\n", ci->ei_block, inode->i_ino);
        memset(kaddr, 0, PAGE_SIZE);
        goto done;
    }

    /* Read the data block */
    bh = sb_bread(inode->i_sb, ci->ei_block);
    if (!bh) {
        pr_err("Failed to read data block %u for inode %lu\n", 
               ci->ei_block, inode->i_ino);
        memset(kaddr, 0, PAGE_SIZE);
        goto done;
    }

    /* Calculate how much to copy - respect file size */
    copy_size = min_t(size_t, inode->i_size, PAGE_SIZE);
    copy_size = min_t(size_t, copy_size, SIMPLEFS_BLOCK_SIZE);
    
    pr_info("Reading file: block=%u, size=%zu, copying %zu bytes\n", 
            ci->ei_block, (size_t)inode->i_size, copy_size);
    
    /* Copy file data */
    if (copy_size > 0) {
        memcpy(kaddr, bh->b_data, copy_size);
        
        /* Debug: show first few bytes */
        pr_info("First 32 bytes: %.32s\n", (char*)bh->b_data);
    }
    
    /* Zero the rest of the page */
    if (copy_size < PAGE_SIZE) {
        memset(kaddr + copy_size, 0, PAGE_SIZE - copy_size);
    }
    
    brelse(bh);

done:
    kunmap(page);
    SetPageUptodate(page);
    unlock_page(page);
    return 0;
}