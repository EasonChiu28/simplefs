/* mkfs.simplefs.c - Updated to match reference structure */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>
#include <endian.h>

#define __le32 uint32_t
#include "simplefs.h"

/* Bitmap manipulation helpers */
static void set_bit(unsigned char *bitmap, int bit)
{
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static void clear_bit(unsigned char *bitmap, int bit)
{
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

int main(int argc, char *argv[])
{
    int fd;
    struct stat st;
    ssize_t ret;
    unsigned char *bitmap_buffer;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <device>\n", argv[0]);
        return 1;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }

    /* Create the content for hello.txt */
    const char *hello_content = "Hello, SimpleFSRF!\nThis is a test file in our custom filesystem.\nIt contains multiple lines of text.\n";
    size_t content_size = strlen(hello_content);

    /* Calculate filesystem parameters */
    uint32_t nr_blocks = st.st_size / SIMPLEFS_BLOCK_SIZE;
    uint32_t nr_inodes = SIMPLEFS_BITS_PER_BLOCK;  /* One bitmap block worth */

    struct simplefs_sb_info sb = {0};
    sb.magic = SIMPLEFS_MAGIC;
    sb.nr_blocks = nr_blocks;
    sb.nr_inodes = nr_inodes;
    sb.nr_free_inodes = nr_inodes - 2;  /* Root dir + hello.txt */
    sb.nr_free_blocks = nr_blocks - 6;  /* superblock + inode table + 2 bitmaps + root dir + hello.txt */
    sb.inode_bitmap_block = SIMPLEFS_INODE_BITMAP_BLOCK;
    sb.block_bitmap_block = SIMPLEFS_BLOCK_BITMAP_BLOCK;
    sb.first_data_block = SIMPLEFS_FIRST_DATA_BLOCK;

    printf("Formatting %s: %u blocks, %u inodes\n", argv[1], sb.nr_blocks, sb.nr_inodes);
    printf("Creating hello.txt with %zu bytes of content\n", content_size);
    printf("Layout: superblock(0) inode_table(1) inode_bitmap(2) block_bitmap(3) data_blocks(4+)\n");

    /* Write superblock to Block 0 */
    lseek(fd, 0, SEEK_SET);
    ret = write(fd, &sb, sizeof(sb));
    if (ret != sizeof(sb)) {
        perror("write superblock");
        close(fd);
        return 1;
    }

    /* Create root directory inode (inode 1) - using simplified structure */
    struct simplefs_inode root_inode = {0};
    root_inode.i_mode = S_IFDIR | 0755;
    root_inode.i_uid = 0;
    root_inode.i_gid = 0;
    root_inode.i_size = SIMPLEFS_BLOCK_SIZE;
    root_inode.i_nlink = 2;
    root_inode.ei_block = 4;  /* Root directory data is in Block 4 */

    /* Write root inode to Block 1 */
    lseek(fd, SIMPLEFS_BLOCK_SIZE, SEEK_SET);
    lseek(fd, sizeof(struct simplefs_inode), SEEK_CUR);  /* Skip inode 0 */
    ret = write(fd, &root_inode, sizeof(root_inode));
    if (ret != sizeof(root_inode)) {
        perror("write root inode");
        close(fd);
        return 1;
    }

    /* Create hello.txt file inode (inode 2) */
    struct simplefs_inode hello_inode = {0};
    hello_inode.i_mode = S_IFREG | 0644;
    hello_inode.i_uid = 0;
    hello_inode.i_gid = 0;
    hello_inode.i_size = content_size;
    hello_inode.i_nlink = 1;
    hello_inode.ei_block = 5;  /* hello.txt data is in Block 5 */

    /* Write hello.txt inode immediately after root inode */
    ret = write(fd, &hello_inode, sizeof(hello_inode));
    if (ret != sizeof(hello_inode)) {
        perror("write hello_inode");
        close(fd);
        return 1;
    }

    /* Create and write inode bitmap to Block 2 */
    bitmap_buffer = calloc(SIMPLEFS_BLOCK_SIZE, 1);
    if (!bitmap_buffer) {
        perror("malloc inode bitmap");
        close(fd);
        return 1;
    }

    /* Mark inodes 1 and 2 as used (inode 0 is reserved) */
    set_bit(bitmap_buffer, 1);  /* Root directory */
    set_bit(bitmap_buffer, 2);  /* hello.txt */

    lseek(fd, SIMPLEFS_BLOCK_SIZE * SIMPLEFS_INODE_BITMAP_BLOCK, SEEK_SET);
    ret = write(fd, bitmap_buffer, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        perror("write inode bitmap");
        free(bitmap_buffer);
        close(fd);
        return 1;
    }

    /* Create and write block bitmap to Block 3 */
    memset(bitmap_buffer, 0, SIMPLEFS_BLOCK_SIZE);
    
    /* Mark used blocks: 0(super), 1(inode table), 2(inode bitmap), 3(block bitmap), 4(root dir), 5(hello.txt) */
    set_bit(bitmap_buffer, 0);  /* Superblock */
    set_bit(bitmap_buffer, 1);  /* Inode table */
    set_bit(bitmap_buffer, 2);  /* Inode bitmap */
    set_bit(bitmap_buffer, 3);  /* Block bitmap */
    set_bit(bitmap_buffer, 4);  /* Root directory data */
    set_bit(bitmap_buffer, 5);  /* hello.txt data */

    lseek(fd, SIMPLEFS_BLOCK_SIZE * SIMPLEFS_BLOCK_BITMAP_BLOCK, SEEK_SET);
    ret = write(fd, bitmap_buffer, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        perror("write block bitmap");
        free(bitmap_buffer);
        close(fd);
        return 1;
    }

    free(bitmap_buffer);

    /* Create root directory data block - using simplified structure */
    struct simplefs_dir_block root_dir_block = {0};
    /* Set number of files */
    
    root_dir_block.nr_files = htole32(1);
    /* Initialize the first file entry */
    root_dir_block.files[0].inode = htole32(2);
    strncpy(root_dir_block.files[0].filename, "hello.txt", SIMPLEFS_FILENAME_LEN);
    
    /* All other entries are already zeroed (inode = 0 means empty) */

    /* Write root directory data to Block 4 */
    lseek(fd, SIMPLEFS_BLOCK_SIZE * 4, SEEK_SET);
    ret = write(fd, &root_dir_block, sizeof(root_dir_block));
    if (ret != sizeof(root_dir_block)) {
        perror("write root dir block");
        close(fd);
        return 1;
    }

    /* Write hello.txt file content to Block 5 */
    lseek(fd, SIMPLEFS_BLOCK_SIZE * 5, SEEK_SET);
    ret = write(fd, hello_content, content_size);
    if (ret != content_size) {
        perror("write hello.txt content");
        close(fd);
        return 1;
    }

    /* Zero out the rest of Block 5 if content doesn't fill the entire block */
    if (content_size < SIMPLEFS_BLOCK_SIZE) {
        size_t remaining = SIMPLEFS_BLOCK_SIZE - content_size;
        char *zero_buffer = calloc(remaining, 1);
        if (zero_buffer) {
            ret = write(fd, zero_buffer, remaining);
            free(zero_buffer);
            if (ret != remaining) {
                perror("write padding zeros");
                close(fd);
                return 1;
            }
        }
    }

    printf("Format complete.\n");
    printf("Filesystem layout:\n");
    printf("  Block 0: Superblock\n");
    printf("  Block 1: Inode table\n");
    printf("  Block 2: Inode bitmap\n");
    printf("  Block 3: Block bitmap\n");
    printf("  Block 4: Root directory data\n");
    printf("  Block 5: hello.txt data\n");
    printf("  Block 6+: Free data blocks\n");
    printf("Created hello.txt with content: \"%.50s%s\"\n", 
           hello_content, strlen(hello_content) > 50 ? "..." : "");
    
    close(fd);
    return 0;
}