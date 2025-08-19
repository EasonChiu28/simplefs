#ifndef SIMPLEFS_H
#define SIMPLEFS_H

#include <linux/types.h>

/* Constants */
#define SIMPLEFS_MAGIC              0xDEADBEEF
#define SIMPLEFS_BLOCK_SIZE         4096
#define SIMPLEFS_FILENAME_LEN       255
#define SIMPLEFS_MAX_SUBFILES       1024
#define SIMPLEFS_INODES_PER_BLOCK   (SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode))

/* Fixed block layout */
#define SIMPLEFS_SUPERBLOCK_BLOCK   0
#define SIMPLEFS_INODE_TABLE_BLOCK  1
#define SIMPLEFS_INODE_BITMAP_BLOCK 2
#define SIMPLEFS_BLOCK_BITMAP_BLOCK 3
#define SIMPLEFS_FIRST_DATA_BLOCK   4

/* Bitmap helpers */
#define SIMPLEFS_BITS_PER_BLOCK     (SIMPLEFS_BLOCK_SIZE * 8)
#define SIMPLEFS_MAX_INODES         SIMPLEFS_BITS_PER_BLOCK
#define SIMPLEFS_MAX_BLOCKS         SIMPLEFS_BITS_PER_BLOCK

/* On-disk data structures */
struct simplefs_sb_info {
    __le32 magic;
    __le32 nr_blocks;
    __le32 nr_inodes;
    __le32 nr_free_blocks;
    __le32 nr_free_inodes;
    __le32 inode_bitmap_block;    /* Block number of inode bitmap */
    __le32 block_bitmap_block;    /* Block number of block bitmap */
    __le32 first_data_block;      /* First data block number */
};

struct simplefs_inode {
    __le32 i_mode;
    __le32 i_uid;
    __le32 i_gid;
    __le32 i_size;
    __le32 i_nlink;
    __le32 ei_block;
};

struct simplefs_file {
    char filename[SIMPLEFS_FILENAME_LEN];
    __le32 inode;
};

struct simplefs_dir_block {
    __le32 nr_files;
    struct simplefs_file files[SIMPLEFS_MAX_SUBFILES];
};

#ifdef __KERNEL__

#include <linux/fs.h>

/* In-memory data structures */
struct simplefs_inode_info {
    __u32 ei_block;
    struct inode vfs_inode;
};

/* Helper Macros */
static inline struct simplefs_sb_info *SIMPLEFS_SB(struct super_block *sb)
{
    return sb->s_fs_info;
}

static inline struct simplefs_inode_info *SIMPLEFS_INODE(struct inode *inode)
{
    return container_of(inode, struct simplefs_inode_info, vfs_inode);
}

/* Bitmap operations */
int simplefs_alloc_inode_num(struct super_block *sb);
void simplefs_free_inode_num(struct super_block *sb, unsigned long ino);
int simplefs_alloc_block_num(struct super_block *sb);
void simplefs_free_block_num(struct super_block *sb, unsigned long block);

/* Function Prototypes */
int simplefs_fill_super(struct super_block *sb, void *data, int silent);
int simplefs_init_inode_cache(void);
void simplefs_destroy_inode_cache(void);
struct inode *simplefs_iget(struct super_block *sb, unsigned long ino);

extern const struct file_operations simplefs_dir_ops;

#endif /* __KERNEL__ */

#endif /* SIMPLEFS_H */