#include <devices/generic_disk.h>
#include <errno.h>
#include <stdint.h>

extern uint64_t PTRS_PER_BLOCK;
#define EXT2_NBLOCKS 15
#define EXT2_SUPERBLOCK_OFFSET 1024
#define EXT2_SIGNATURE_OFFSET 0x38
#define EXT2_SIGNATURE 0xEF53
#define EXT2_NAME_LEN 255
#define EXT2_ROOT_INODE 2
#define EXT2_S_IFSOCK 0xC000 // socket
#define EXT2_S_IFLNK 0xA000  // symbolic link
#define EXT2_S_IFREG 0x8000  // regular file
#define EXT2_S_IFBLK 0x6000  // block device
#define EXT2_S_IFDIR 0x4000  // directory
#define EXT2_S_IFCHR 0x2000  // character device
#define EXT2_S_IFIFO 0x1000  // FIFO
#define EXT2_S_IFMT 0xF000   // mask to extract file type from i_mode
#define EXT2_FT_UNKNOWN 0    // Unknown file type
#define EXT2_FT_REG_FILE 1   // Regular file
#define EXT2_FT_DIR 2        // Directory
#define EXT2_FT_CHRDEV 3     // Character device
#define EXT2_FT_BLKDEV 4     // Block device
#define EXT2_FT_FIFO 5       // FIFO (named pipe)
#define EXT2_FT_SOCK 6       // Unix domain socket
#define EXT2_FT_SYMLINK 7    // Symbolic link
#define EXT2_FT_MAX 8        // Number of defined file types
#define MIN(x, y) ((x > y) ? y : x)

#define MAKE_NOP_CALLBACK                                                      \
    static bool nop_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry, \
                             void *ctx_ptr, uint32_t block_num,                \
                             uint32_t entry_num, uint32_t entry_offset) {      \
        (void) fs;                                                             \
        (void) entry_offset;                                                   \
        (void) entry;                                                          \
        (void) ctx_ptr;                                                        \
        (void) block_num;                                                      \
        (void) entry_num;                                                      \
        return false;                                                          \
    }

struct ext2_sblock {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t r_blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
    char last_mounted[64];
    uint32_t algorithm_usage_bitmap;
    uint8_t prealloc_blocks;
    uint8_t prealloc_dir_blocks;
    uint16_t padding;

    union {
        struct {
            uint32_t journal_uuid[4];
            uint32_t journal_inum;
            uint32_t journal_dev;
            uint32_t last_orphan;
            uint32_t hash_seed[4];
            uint8_t def_hash_version;
            uint8_t journal_backup_type;
            uint16_t desc_size;
            uint32_t default_mount_opts;
            uint32_t first_meta_bg;
            uint32_t mkfs_time;
            uint32_t journal_blocks[17];
            uint32_t quota_group_inode;   // [4] → offset 0x258
            uint32_t quota_project_inode; // [5] → offset 0x25C
        };
        uint32_t reserved[204];
    };
} __attribute__((packed));

struct ext2_group_desc {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint32_t reserved[3];
} __attribute__((__packed__));

struct ext2_inode {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t flags;
    uint32_t osd1;

    uint32_t block[EXT2_NBLOCKS];
    uint32_t generation;
    uint32_t file_acl;
    uint32_t dir_acl;
    uint32_t faddr;
    uint8_t frag[16];
    uint8_t osd2[12];
} __attribute__((packed));

struct k_full_inode {
    struct ext2_inode node;
    uint32_t inode_num;
};

struct ext2_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[EXT2_NAME_LEN + 1];
} __attribute__((packed));

struct ext2_fs {
    struct generic_disk *drive;
    struct ext2_sblock *sblock;
    struct ext2_group_desc *group_desc;
    uint32_t num_groups;
    uint32_t inodes_count;
    uint32_t inodes_per_group;
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint16_t inode_size;
};

typedef bool (*dir_entry_callback)(struct ext2_fs *fs,
                                   struct ext2_dir_entry *entry, void *ctx,
                                   uint32_t block_num, uint32_t entry_num,
                                   uint32_t entry_offset);

typedef void (*ext2_block_visitor)(struct ext2_fs *fs, struct ext2_inode *inode,
                                   uint32_t depth, uint32_t *block_ptr,
                                   void *user_data);

//
//
// R/W + Mount
//
//

bool ext2_block_read(struct generic_disk *d, uint32_t lba, uint8_t *buffer,
                     uint32_t sector_count);

bool ext2_block_ptr_read(struct ext2_fs *fs, uint32_t block_num, void *buf);

bool ext2_block_write(struct generic_disk *d, uint32_t lba,
                      const uint8_t *buffer, uint32_t sector_count);

bool ext2_block_ptr_write(struct ext2_fs *fs, uint32_t block_num, void *buf);

bool ext2_read_superblock(struct generic_disk *d, uint32_t partition_start_lba,
                          struct ext2_sblock *sblock);

bool ext2_write_superblock(struct ext2_fs *fs);
bool ext2_write_group_desc(struct ext2_fs *fs);

enum errno ext2_mount(struct generic_disk *d, struct ext2_fs *fs,
                      struct ext2_sblock *sblock);
enum errno ext2_g_mount(struct generic_disk *d);

bool ext2_read_inode(struct ext2_fs *fs, uint32_t inode_idx,
                     struct ext2_inode *inode_out);

bool ext2_write_inode(struct ext2_fs *fs, uint32_t inode_num,
                      const struct ext2_inode *inode);

uint32_t ext2_get_or_set_block(struct ext2_fs *fs, struct ext2_inode *inode,
                               uint32_t block_index, uint32_t new_block_num,
                               bool allocate, bool *was_allocated);

//
//
// Higher level stuff
//
//

enum errno ext2_link_file(struct ext2_fs *fs, struct k_full_inode *dir_inode,
                          struct k_full_inode *inode, char *name);

enum errno ext2_unlink_file(struct ext2_fs *fs, struct k_full_inode *dir_inode,
                            const char *name);

enum errno ext2_create_file(struct ext2_fs *fs, struct k_full_inode *parent_dir,
                            const char *name, uint16_t mode);

enum errno ext2_symlink_file(struct ext2_fs *fs, struct k_full_inode *dir_inode,
                             const char *name, char *target);

enum errno ext2_write_file(struct ext2_fs *fs, struct k_full_inode *inode,
                           uint32_t offset, const uint8_t *src, uint32_t size);

struct k_full_inode *ext2_find_file_in_dir(struct ext2_fs *fs,
                                           struct k_full_inode *dir_inode,
                                           const char *fname);

bool ext2_dir_contains_file(struct ext2_fs *fs, struct k_full_inode *dir_inode,
                            const char *fname);

//
//
//
// Alloc/dealloc
//
//

uint32_t ext2_alloc_block(struct ext2_fs *fs);
bool ext2_free_block(struct ext2_fs *fs, uint32_t block_num);
uint32_t ext2_alloc_inode(struct ext2_fs *fs);
bool ext2_free_inode(struct ext2_fs *fs, uint32_t inode_num);

bool ext2_walk_dir(struct ext2_fs *fs, struct k_full_inode *dir_inode,
                   dir_entry_callback cb, void *ctx, bool ff_avail);

void ext2_traverse_inode_blocks(struct ext2_fs *fs, struct ext2_inode *inode,
                                ext2_block_visitor visitor, void *user_data);

void ext2_test(struct generic_disk *d, struct ext2_sblock *sblock);
void ext2_g_print(struct generic_disk *d);
#pragma once
