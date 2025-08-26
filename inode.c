/* inode.c - Enhanced with forced disk persistence for mkdir */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/blkdev.h>

#include "simplefs.h"

static struct dentry *simplefs_lookup(struct inode *dir,
                                      struct dentry *dentry,
                                      unsigned int flags);
static int simplefs_readpage(struct file *file, struct page *page);
static int simplefs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode);

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

/* inode operations with mkdir support */
static const struct inode_operations simplefs_inode_ops = {
    .lookup = simplefs_lookup,
    .mkdir = simplefs_mkdir,
};

const struct file_operations simplefs_file_ops = {
    .owner = THIS_MODULE,
    .read_iter = generic_file_read_iter,
    .llseek = generic_file_llseek,
};

const struct address_space_operations simplefs_aops = {
    .readpage = simplefs_readpage,
};

struct inode *simplefs_iget(struct super_block *sb, unsigned long ino)
{
    struct inode *inode;
    struct simplefs_inode *raw_inode;
    struct simplefs_inode_info *ci;
    struct buffer_head *bh;
    uint32_t inode_block, inode_shift;
    uint32_t i_mode, i_size, ei_block, i_nlink;

    pr_info("Loading inode %lu\n", ino);

    /* Validate inode number */
    if (ino == 0 || ino >= SIMPLEFS_MAX_INODES) {
        pr_err("Invalid inode number %lu\n", ino);
        return ERR_PTR(-EINVAL);
    }

    inode_block = (ino / SIMPLEFS_INODES_PER_BLOCK) + 1;
    inode_shift = ino % SIMPLEFS_INODES_PER_BLOCK;

    inode = iget_locked(sb, ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW))
        return inode;

    ci = SIMPLEFS_INODE(inode);

    /* Validate inode block number */
    if (inode_block >= 4) { /* Should be block 1 for our simple filesystem */
        pr_err("Invalid inode block %u for inode %lu\n", inode_block, ino);
        iget_failed(inode);
        return ERR_PTR(-EIO);
    }

    bh = sb_bread(sb, inode_block);
    if (!bh) {
        pr_err("Failed to read inode block %u\n", inode_block);
        iget_failed(inode);
        return ERR_PTR(-EIO);
    }
    
    raw_inode = (struct simplefs_inode *)bh->b_data;
    raw_inode += inode_shift;

    /* Validate raw inode data before using it */
    i_mode = le32_to_cpu(raw_inode->i_mode);
    i_size = le32_to_cpu(raw_inode->i_size);
    ei_block = le32_to_cpu(raw_inode->ei_block);
    i_nlink = le32_to_cpu(raw_inode->i_nlink);

    /* Basic sanity checks */
    if (!S_ISDIR(i_mode) && !S_ISREG(i_mode)) {
        pr_err("Invalid inode mode 0x%x for inode %lu\n", i_mode, ino);
        brelse(bh);
        iget_failed(inode);
        return ERR_PTR(-EINVAL);
    }

    if (ei_block >= 12800) { /* Reasonable upper bound for our filesystem */
        pr_err("Invalid data block %u for inode %lu\n", ei_block, ino);
        brelse(bh);
        iget_failed(inode);
        return ERR_PTR(-EINVAL);
    }

    /* Copy validated data to VFS inode */
    inode->i_mode = i_mode;
    i_uid_write(inode, le32_to_cpu(raw_inode->i_uid));
    i_gid_write(inode, le32_to_cpu(raw_inode->i_gid));
    inode->i_size = i_size;
    set_nlink(inode, i_nlink);
    ci->ei_block = ei_block;

    /* Set timestamps to prevent issues */
    inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

    /* Initialize additional inode fields */
    inode->i_blocks = 1;
    inode->i_blkbits = sb->s_blocksize_bits;
    
    /* For simplicity, disable atime updates */
    inode->i_flags |= S_NOATIME;

    pr_info("Inode %lu: mode=0x%x, size=%lld, block=%u, nlink=%u\n", 
             ino, inode->i_mode, inode->i_size, ci->ei_block, inode->i_nlink);

    if (S_ISDIR(inode->i_mode)) {
        pr_info("Setting up directory inode %lu\n", ino);
        inode->i_op = &simplefs_inode_ops;
        inode->i_fop = &simplefs_dir_ops;
        /* Ensure directory size is reasonable */
        if (inode->i_size == 0) {
            inode->i_size = SIMPLEFS_BLOCK_SIZE;
        }
        pr_info("Directory inode %lu: block=%u, fop=%p, size=%lld\n", 
                ino, ci->ei_block, inode->i_fop, inode->i_size);
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

    /* Validate directory block */
    if (ci_dir->ei_block == 0 || ci_dir->ei_block >= 12800) {
        pr_err("Invalid directory block %u\n", ci_dir->ei_block);
        return ERR_PTR(-EIO);
    }

    /* Validate filename length */
    if (dentry->d_name.len >= SIMPLEFS_FILENAME_LEN) {
        pr_err("Filename too long: %u\n", dentry->d_name.len);
        return ERR_PTR(-ENAMETOOLONG);
    }

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
        
        /* Validate inode number */
        if (ino == 0 || ino >= SIMPLEFS_MAX_INODES) {
            pr_warn("Invalid inode %u for file '%s'\n", ino, f->filename);
            continue;
        }
        
        if (strcmp(dentry->d_name.name, f->filename) == 0) {
            pr_info("Found match: '%s' -> inode %u\n", f->filename, ino);
            inode = simplefs_iget(sb, ino);
            if (IS_ERR(inode)) {
                pr_err("Failed to load inode %u: %ld\n", ino, PTR_ERR(inode));
                inode = NULL;
            }
            break;
        }
    }
    brelse(bh);

    pr_info("Lookup complete: '%s' -> %s\n", dentry->d_name.name, 
            inode ? "found" : "not found");
    return d_splice_alias(inode, dentry);
}

static int simplefs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
    struct super_block *sb = dir->i_sb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct simplefs_inode_info *ci_dir = SIMPLEFS_INODE(dir);
    struct buffer_head *bh = NULL;
    struct simplefs_dir_block *dblock;
    struct simplefs_file *new_entry;
    uint32_t nr_files;
    int new_ino, new_block;
    int ret = 0;
    int i;
    
    pr_info("mkdir: Creating directory '%s' in parent inode %lu\n", 
             dentry->d_name.name, dir->i_ino);

    /* Basic validation */
    if (dentry->d_name.len >= SIMPLEFS_FILENAME_LEN) {
        pr_err("Directory name too long: %u\n", dentry->d_name.len);
        return -ENAMETOOLONG;
    }

    /* Check if filesystem has space */
    if (sbi->nr_free_inodes == 0) {
        pr_err("No free inodes available\n");
        return -ENOSPC;
    }

    if (sbi->nr_free_blocks == 0) {
        pr_err("No free blocks available\n");
        return -ENOSPC;
    }

    /* Allocate resources */
    new_ino = simplefs_alloc_inode_num(sb);
    if (new_ino < 0) {
        pr_err("Failed to allocate inode: %d\n", new_ino);
        return new_ino;
    }

    new_block = simplefs_alloc_block_num(sb);
    if (new_block < 0) {
        pr_err("Failed to allocate block: %d\n", new_block);
        simplefs_free_inode_num(sb, new_ino);
        return new_block;
    }

    pr_info("mkdir: Allocated inode %d, block %d (both written to disk)\n", 
            new_ino, new_block);

    /* Read parent directory */
    bh = sb_bread(sb, ci_dir->ei_block);
    if (!bh) {
        pr_err("Failed to read parent directory\n");
        ret = -EIO;
        goto cleanup;
    }

    dblock = (struct simplefs_dir_block *)bh->b_data;
    nr_files = le32_to_cpu(dblock->nr_files);

    /* Check space and existing files */
    if (nr_files >= SIMPLEFS_MAX_SUBFILES) {
        pr_err("Parent directory is full\n");
        ret = -ENOSPC;
        goto cleanup;
    }

    /* Check if name already exists */
    for (i = 0; i < nr_files; i++) {
        struct simplefs_file *f = &dblock->files[i];
        f->filename[SIMPLEFS_FILENAME_LEN - 1] = '\0';
        if (strcmp(f->filename, dentry->d_name.name) == 0) {
            pr_err("Directory '%s' already exists\n", dentry->d_name.name);
            ret = -EEXIST;
            goto cleanup;
        }
    }

    /* Add entry to parent directory */
    new_entry = &dblock->files[nr_files];
    memset(new_entry, 0, sizeof(*new_entry));
    strncpy(new_entry->filename, dentry->d_name.name, SIMPLEFS_FILENAME_LEN - 1);
    new_entry->filename[SIMPLEFS_FILENAME_LEN - 1] = '\0';
    new_entry->inode = cpu_to_le32(new_ino);
    dblock->nr_files = cpu_to_le32(nr_files + 1);

    /* Force write parent directory to disk */
    ret = force_write_buffer(bh);
    brelse(bh);
    bh = NULL;
    
    if (ret) {
        pr_err("Failed to write parent directory to disk: %d\n", ret);
        goto cleanup;
    }
    
    pr_info("mkdir: Parent directory updated and written to disk\n");

    /* Create the directory inode on disk */
    {
        struct buffer_head *inode_bh;
        struct simplefs_inode *raw_inode;
        uint32_t inode_block = (new_ino / SIMPLEFS_INODES_PER_BLOCK) + 1;
        uint32_t inode_shift = new_ino % SIMPLEFS_INODES_PER_BLOCK;

        pr_info("mkdir: Writing directory inode %d to block %u, shift %u\n", 
                new_ino, inode_block, inode_shift);

        inode_bh = sb_bread(sb, inode_block);
        if (!inode_bh) {
            pr_err("Failed to read inode block for new directory\n");
            ret = -EIO;
            goto cleanup;
        }

        raw_inode = (struct simplefs_inode *)inode_bh->b_data;
        raw_inode += inode_shift;

        /* Initialize the directory inode */
        raw_inode->i_mode = cpu_to_le32(S_IFDIR | 0755);
        raw_inode->i_uid = cpu_to_le32(0);
        raw_inode->i_gid = cpu_to_le32(0);
        raw_inode->i_size = cpu_to_le32(SIMPLEFS_BLOCK_SIZE);
        raw_inode->i_nlink = cpu_to_le32(2); /* . and .. */
        raw_inode->ei_block = cpu_to_le32(new_block);

        /* Force write inode to disk */
        ret = force_write_buffer(inode_bh);
        brelse(inode_bh);
        
        if (ret) {
            pr_err("Failed to write directory inode to disk: %d\n", ret);
            goto cleanup;
        }

        pr_info("mkdir: Directory inode written to disk successfully\n");
    }

    /* Initialize the directory data block */
    {
        struct buffer_head *data_bh;
        struct simplefs_dir_block *dir_data;

        pr_info("mkdir: Initializing directory data block %d\n", new_block);

        data_bh = sb_bread(sb, new_block);
        if (!data_bh) {
            pr_err("Failed to read data block for new directory\n");
            ret = -EIO;
            goto cleanup;
        }

        dir_data = (struct simplefs_dir_block *)data_bh->b_data;
        memset(dir_data, 0, sizeof(*dir_data));
        dir_data->nr_files = cpu_to_le32(0); /* Empty directory */

        /* Force write directory data to disk */
        ret = force_write_buffer(data_bh);
        brelse(data_bh);
        
        if (ret) {
            pr_err("Failed to write directory data to disk: %d\n", ret);
            goto cleanup;
        }

        pr_info("mkdir: Directory data block initialized and written to disk\n");
    }

    pr_info("mkdir: Successfully created directory '%s' with inode %d, block %d - all data written to disk\n", 
            dentry->d_name.name, new_ino, new_block);
    return 0;

cleanup:
    if (bh) {
        brelse(bh);
    }
    simplefs_free_block_num(sb, new_block);
    simplefs_free_inode_num(sb, new_ino);
    pr_err("mkdir: Failed to create directory '%s', cleaned up resources\n", 
           dentry->d_name.name);
    return ret;
}

static int simplefs_readpage(struct file *file, struct page *page)
{
    struct inode *inode = file_inode(file);
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct buffer_head *bh;
    void *kaddr;
    size_t copy_size;

    pr_info("Reading page for inode %lu, block %u, size %lld\n", 
            inode->i_ino, ci->ei_block, inode->i_size);

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

    /* Validate block number */
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