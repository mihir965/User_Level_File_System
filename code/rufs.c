/*
 *  Copyright (C) 2025 CS416 Rutgers CS
 *	Rutgers Tiny File System
 *	File:	rufs.c
 *
 */

#include <time.h>
#define FUSE_USE_VERSION 26

#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "block.h"
#include "rufs.h"

char diskfile_path[PATH_MAX];

// Declare your in-memory data structures here
//
// We declare it for quick access to disk layout information

struct superblock sb;

/* Constants that define how many inodes and directories can be there per block
 */
#define INODES_PER_BLOCK (BLOCK_SIZE / sizeof(struct inode))

#define DIRENTS_PER_BLOCK (BLOCK_SIZE / sizeof(struct dirent))

/*
 * Get available inode number from bitmap
 */
int get_avail_ino() {

  // Step 1: Read inode bitmap from disk

  /* The bitmap will fit in one block (1024 inodes = 128 bytes needed, block =
   * 4096 bytes) */
  bitmap_t inode_bitmap = (bitmap_t)malloc(BLOCK_SIZE);
  bio_read(sb.i_bitmap_blk, inode_bitmap);

  // Step 2: Traverse inode bitmap to find an available slot
  for (int i = 0; i < MAX_INUM; i++) {
    if (get_bitmap(inode_bitmap, i) == 0) {
      /* Found the free inode */
      set_bitmap(inode_bitmap, i);
      bio_write(sb.i_bitmap_blk, inode_bitmap);

      // Step 3: Update inode bitmap and write to disk
      free(inode_bitmap);
      return i;
    }
  }

  /* No free inodes available */
  free(inode_bitmap);
  return -1;
}

/*
 * Get available data block number from bitmap
 */
int get_avail_blkno() {

  // Step 1: Read data block bitmap from disk

  bitmap_t data_bitmap = (bitmap_t)malloc(BLOCK_SIZE);
  bio_read(sb.d_bitmap_blk, data_bitmap);

  // Step 2: Traverse data block bitmap to find an available slot

  for (int i = 0; i < MAX_DNUM; i++) {
    if (get_bitmap(data_bitmap, i) == 0) {
      set_bitmap(data_bitmap, i);
      bio_write(sb.d_bitmap_blk, data_bitmap);

      // Step 3: Update data block bitmap and write to disk
      free(data_bitmap);
      /*return the actual block number on disk */
      return i;
    }
  }

  free(data_bitmap);
  return -1;
}

/*
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {

  // Step 1: Get the inode's on-disk block number
  int block_num = sb.i_start_blk + (ino / INODES_PER_BLOCK);

  // Step 2: Get offset of the inode in the inode on-disk block
  int offset = ino % INODES_PER_BLOCK;

  // Step 3: Read the block from disk and then copy into inode structure
  struct inode *inode_block = (struct inode *)malloc(BLOCK_SIZE);
  bio_read(block_num, inode_block);

  *inode = inode_block[offset];

  free(inode_block);
  return 0;
}

int writei(uint16_t ino, struct inode *inode) {

  // Step 1: Get the block number where this inode resides on disk
  int block_num = sb.i_start_blk + (ino / INODES_PER_BLOCK);
  // Step 2: Get the offset in the block where this inode resides on disk
  int offset = ino % INODES_PER_BLOCK;
  // Step 3: Write inode to disk
  struct inode *inode_block = (struct inode *)malloc(BLOCK_SIZE);
  bio_read(block_num, inode_block);

  /* Update the specifict inode in the block */
  inode_block[offset] = *inode;
  bio_write(block_num, inode_block);
  free(inode_block);
  return 0;
}

/*
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len,
             struct dirent *dirent) {

  // Step 1: Call readi() to get the inode using ino (inode number of current
  // directory)
  struct inode dir_inode;
  readi(ino, &dir_inode);

  // Step 2: Get data block of current directory from inode
  struct dirent *entries = (struct dirent *)malloc(BLOCK_SIZE);

  for (int i = 0; i < 16; i++) {
    /* Check if this direct pointer is valid (not -1 or 0) */
    if (dir_inode.direct_ptr[i] == -1)
      continue;
    bio_read(sb.d_start_blk + dir_inode.direct_ptr[i], entries);

    // Step 3: Read directory's data block and check each directory entry.
    // If the name matches, then copy directory entry to dirent structure
    for (int j = 0; j < DIRENTS_PER_BLOCK; j++) {
      if (entries[j].valid == 1) {
        if (strcmp(entries[j].name, fname) == 0) {
          /* Found it */
          *dirent = entries[j];
          free(entries);
          return 0;
        }
      }
    }
  }

  free(entries);
  return -1;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname,
            size_t name_len) {

  // Step 1: Read dir_inode's data block and check each directory entry of
  // dir_inode
  struct dirent *entries = (struct dirent *)malloc(BLOCK_SIZE);

  // Step 2: Check if fname (directory name) is already used in other entries
  for (int i = 0; i < 16; i++) {
    if (dir_inode.direct_ptr[i] == -1)
      continue;
    bio_read(sb.d_start_blk + dir_inode.direct_ptr[i], entries);

    for (int j = 0; j < DIRENTS_PER_BLOCK; j++) {
      if (entries[j].valid && strcmp(entries[j].name, fname) == 0) {
        /* Name already exists */
        free(entries);
        return -EEXIST;
      }
    }
  }
  // Step 3: Add directory entry in dir_inode's data block and write to disk
  for (int i = 0; i < 16; i++) {
    if (dir_inode.direct_ptr[i] == -1)
      continue;
    bio_read(sb.d_start_blk + dir_inode.direct_ptr[i], entries);
    for (int j = 0; j < DIRENTS_PER_BLOCK; j++) {
      if (!entries[j].valid) {
        /* Found an empty slot */
        entries[j].valid = 1;
        entries[j].ino = f_ino;
        strncpy(entries[j].name, fname, sizeof(entries[j].name) - 1);
        entries[j].name[sizeof(entries[j].name) - 1] = '\0';
        entries[j].len = name_len;

        bio_write(sb.d_start_blk + dir_inode.direct_ptr[i], entries);
        free(entries);
        return 0;
      }
    }
  }
  /* No empty slots found = need to allocate a new block */
  for (int i = 0; i < 16; i++) {
    if (dir_inode.direct_ptr[i] == -1) {
      // Allocate a new data block
      int new_blkno = get_avail_blkno();
      if (new_blkno == -1) {
        free(entries);
        return -ENOSPC; // No space left
      }

      dir_inode.direct_ptr[i] = new_blkno;

      // Initialize the new block with empty entries
      memset(entries, 0, BLOCK_SIZE);
      for (int k = 0; k < DIRENTS_PER_BLOCK; k++) {
        entries[k].valid = 0;
      }

      // Add our new entry
      entries[0].valid = 1;
      entries[0].ino = f_ino;
      strncpy(entries[0].name, fname, sizeof(entries[0].name) - 1);
      entries[0].name[sizeof(entries[0].name) - 1] = '\0';
      entries[0].len = name_len;

      // Write the new data block
      bio_write(sb.d_start_blk + new_blkno, entries);

      // Update directory inode size and write it back
      dir_inode.size += BLOCK_SIZE;
      writei(dir_inode.ino, &dir_inode);

      free(entries);
      return 0;
    }
  };

  free(entries);
  return -ENOSPC;
}

/*
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {

  // Step 1: Resolve the path name, walk through path, and finally, find its
  // inode. Note: You could either implement it in a iterative way or recursive
  // way

  /* Root directory */
  if (strcmp(path, "/") == 0)
    return readi(0, inode);

  char *path_copy = strdup(path);
  char *token;
  uint16_t current_ino = 0;
  struct dirent entry;

  token = strtok(path_copy, "/");

  while (token != NULL) {
    if (dir_find(current_ino, token, strlen(token), &entry) != 0) {
      free(path_copy);
      return -1;
    }

    current_ino = entry.ino;
    token = strtok(NULL, "/");
  }

  readi(current_ino, inode);
  free(path_copy);
  return 0;
}

/*
 * Make file system
 */
int rufs_mkfs() {

  // Call dev_init() to initialize (Create) Diskfile
  dev_init(diskfile_path);
  // write superblock information
  sb.magic_num = MAGIC_NUM;
  sb.max_inum = MAX_INUM;
  sb.max_dnum = MAX_DNUM;
  sb.i_bitmap_blk = 1;
  sb.d_bitmap_blk = 2;
  sb.i_start_blk = 3;

  // initialize inode bitmap
  int inode_blocks = (MAX_INUM + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK;
  sb.d_start_blk = sb.i_start_blk + inode_blocks;

  bio_write(0, &sb);

  bitmap_t inode_bitmap = (bitmap_t)calloc(1, BLOCK_SIZE);
  bio_write(sb.i_bitmap_blk, inode_bitmap);

  bitmap_t data_bitmap = (bitmap_t)calloc(1, BLOCK_SIZE);
  bio_write(sb.d_bitmap_blk, data_bitmap);

  set_bitmap(inode_bitmap, 0);
  bio_write(sb.i_bitmap_blk, inode_bitmap);

  set_bitmap(data_bitmap, 0);
  bio_write(sb.d_bitmap_blk, data_bitmap);

  struct inode root_inode;
  memset(&root_inode, 0, sizeof(struct inode));
  root_inode.ino = 0;
  root_inode.valid = 1;
  root_inode.size = BLOCK_SIZE; // One data block
  root_inode.type = S_IFDIR;    // Directory type
  root_inode.link = 2;          // . and parent (for root, parent is itself)

  // Initialize all direct pointers to -1 (invalid)
  for (int i = 0; i < 16; i++) {
    root_inode.direct_ptr[i] = -1;
  }
  for (int i = 0; i < 8; i++) {
    root_inode.indirect_ptr[i] = -1;
  }

  // Root directory uses data block 0
  root_inode.direct_ptr[0] = 0;

  // Set stat information
  root_inode.vstat.st_mode = S_IFDIR | 0755;
  root_inode.vstat.st_nlink = 2;
  root_inode.vstat.st_uid = getuid();
  root_inode.vstat.st_gid = getgid();
  root_inode.vstat.st_size = BLOCK_SIZE;
  time(&root_inode.vstat.st_atime);
  time(&root_inode.vstat.st_mtime);

  // Write root inode to disk
  writei(0, &root_inode);

  // Initialize root directory's data block (empty directory entries)
  struct dirent *root_entries = (struct dirent *)calloc(1, BLOCK_SIZE);
  for (int i = 0; i < DIRENTS_PER_BLOCK; i++) {
    root_entries[i].valid = 0;
  }
  bio_write(sb.d_start_blk + 0, root_entries);

  free(inode_bitmap);
  free(data_bitmap);
  free(root_entries);

  return 0;
}

/*
 * FUSE file operations
 */
static void *rufs_init(struct fuse_conn_info *conn) {
  // Step 1a: If disk file is not found, call mkfs
  if (dev_open(diskfile_path) != 0)
    rufs_mkfs();
  else
    // Step 1b: If disk file is found, just initialize in-memory data structures
    // and read superblock from disk
    bio_read(0, &sb);
  return NULL;
}

static void rufs_destroy(void *userdata) {

  // Step 1: De-allocate in-memory data structures

  // Step 2: Close diskfile
  dev_close();
}

static int rufs_getattr(const char *path, struct stat *stbuf) {

  // Step 1: call get_node_by_path() to get inode from path
  struct inode node;

  if (get_node_by_path(path, 0, &node) != 0)
    /* Path not found */
    return -ENOENT;
  // Step 2: fill attribute of file into stbuf from inode
  memset(stbuf, 0, sizeof(struct stat));

  if (node.type == S_IFDIR) {
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
  } else {
    stbuf->st_mode = S_IFREG | 0644;
    stbuf->st_nlink = 1;
  }

  stbuf->st_uid = getuid();
  stbuf->st_gid = getgid();
  stbuf->st_size = node.size;
  stbuf->st_atime = node.vstat.st_atime;
  stbuf->st_mtime = node.vstat.st_mtime;

  return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi) {

  // Step 1: Call get_node_by_path() to get inode from path
  struct inode dir_inode;

  // Step 2: If not find, return -1
  if (get_node_by_path(path, 0, &dir_inode) != 0)
    return -ENOENT;
  return 0;
}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi) {

  // Step 1: Call get_node_by_path() to get inode from path
  struct inode dir_inode;

  if (get_node_by_path(path, 0, &dir_inode) != 0)
    return -ENOENT;

  filler(buffer, ".", NULL, 0);
  filler(buffer, "..", NULL, 0);
  // Step 2: Read directory entries from its data blocks, and copy them to
  // filler
  struct dirent *entries = (struct dirent *)malloc(BLOCK_SIZE);

  for (int i = 0; i < 16; i++) {
    if (dir_inode.direct_ptr[i] == -1)
      continue;
    bio_read(sb.d_start_blk + dir_inode.direct_ptr[i], entries);

    for (int j = 0; j < DIRENTS_PER_BLOCK; j++) {
      if (entries[j].valid == 1)
        filler(buffer, entries[j].name, NULL, 0);
    }
  }
  free(entries);
  return 0;
}

static int rufs_mkdir(const char *path, mode_t mode) {

  // Step 1: Use dirname() and basename() to separate parent directory path and
  // target directory name

  char *path_copy1 = strdup(path);
  char *path_copy2 = strdup(path);

  char *dir = dirname(path_copy1);
  char *base = basename(path_copy2);

  // Step 2: Call get_node_by_path() to get inode of parent directory
  struct inode parent_node;
  if (get_node_by_path(dir, 0, &parent_node) != 0) {
    free(path_copy1);
    free(path_copy2);
    return -ENOENT;
  }
  // Step 3: Call get_avail_ino() to get an available inode number
  int new_ino = get_avail_ino();
  if (new_ino == -1) {
    free(path_copy1);
    free(path_copy2);
    return -ENOSPC;
  }
  // Step 4: Call dir_add() to add directory entry of target directory to parent
  // directory
  if (dir_add(parent_node, new_ino, base, strlen(base)) != 0) {
    free(path_copy1);
    free(path_copy2);
    return -EEXIST;
  }
  // Step 5: Update inode for target directory
  struct inode new_dir_inode;
  memset(&new_dir_inode, 0, sizeof(struct inode));
  new_dir_inode.ino = new_ino;
  new_dir_inode.valid = 1;
  new_dir_inode.type = S_IFDIR;
  new_dir_inode.link = 2;

  // Initialize direct pointers to invalid
  for (int i = 0; i < 16; i++) {
    new_dir_inode.direct_ptr[i] = -1;
  }
  for (int i = 0; i < 8; i++) {
    new_dir_inode.indirect_ptr[i] = -1;
  }

  // Allocate a data block for the new directory
  int new_blkno = get_avail_blkno();
  if (new_blkno == -1) {
    free(path_copy1);
    free(path_copy2);
    return -ENOSPC;
  }
  new_dir_inode.direct_ptr[0] = new_blkno;
  new_dir_inode.size = BLOCK_SIZE;

  // Initialize the new directory's data block (empty entries)
  struct dirent *entries = (struct dirent *)calloc(1, BLOCK_SIZE);
  for (int i = 0; i < DIRENTS_PER_BLOCK; i++) {
    entries[i].valid = 0;
  }
  bio_write(sb.d_start_blk + new_blkno, entries);
  free(entries);

  // Set stat information
  new_dir_inode.vstat.st_mode = S_IFDIR | 0755;
  new_dir_inode.vstat.st_nlink = 2;
  new_dir_inode.vstat.st_uid = getuid();
  new_dir_inode.vstat.st_gid = getgid();
  new_dir_inode.vstat.st_size = BLOCK_SIZE;
  time(&new_dir_inode.vstat.st_atime);
  time(&new_dir_inode.vstat.st_mtime);

  // Step 6: Call writei() to write inode to disk
  writei(new_ino, &new_dir_inode);

  free(path_copy1);
  free(path_copy2);
  return 0;
}

static int rufs_create(const char *path, mode_t mode,
                       struct fuse_file_info *fi) {

  // Step 1: Use dirname() and basename() to separate parent directory path and
  // target file name
  char *path_copy1 = strdup(path);
  char *path_copy2 = strdup(path);
  char *dir = dirname(path_copy1);
  char *base = basename(path_copy2);

  // Step 2: Call get_node_by_path() to get inode of parent directory
  struct inode parent_inode;
  if (get_node_by_path(dir, 0, &parent_inode) != 0) {
    free(path_copy1);
    free(path_copy2);
    return -ENOENT;
  }
  // Step 3: Call get_avail_ino() to get an available inode number
  int new_ino = get_avail_ino();
  if (new_ino == -1) {
    free(path_copy1);
    free(path_copy2);
    return -ENOSPC;
  }
  // Step 4: Call dir_add() to add directory entry of target file to parent
  // directory
  if (dir_add(parent_inode, new_ino, base, strlen(base)) != 0) {
    free(path_copy1);
    free(path_copy2);
    return -EEXIST;
  }
  // Step 5: Update inode for target file
  struct inode new_file_inode;
  memset(&new_file_inode, 0, sizeof(struct inode));
  new_file_inode.ino = new_ino;
  new_file_inode.valid = 1;
  new_file_inode.type = S_IFREG; // Regular file
  new_file_inode.link = 1;
  new_file_inode.size = 0; // Empty file initially

  // Initialize direct pointers to invalid
  for (int i = 0; i < 16; i++) {
    new_file_inode.direct_ptr[i] = -1;
  }
  for (int i = 0; i < 8; i++) {
    new_file_inode.indirect_ptr[i] = -1;
  }

  // Set stat information
  new_file_inode.vstat.st_mode = S_IFREG | 0644;
  new_file_inode.vstat.st_nlink = 1;
  new_file_inode.vstat.st_uid = getuid();
  new_file_inode.vstat.st_gid = getgid();
  new_file_inode.vstat.st_size = 0;
  time(&new_file_inode.vstat.st_atime);
  time(&new_file_inode.vstat.st_mtime);

  // Step 6: Call writei() to write inode to disk
  writei(new_ino, &new_file_inode);

  free(path_copy1);
  free(path_copy2);
  return 0;
}

static int rufs_open(const char *path, struct fuse_file_info *fi) {

  // Step 1: Call get_node_by_path() to get inode from path
  struct inode file_inode;
  // Step 2: If not find, return -1
  if (get_node_by_path(path, 0, &file_inode) != 0)
    return -ENOENT;
  return 0;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset,
                     struct fuse_file_info *fi) {

  // Step 1: You could call get_node_by_path() to get inode from path
  struct inode file_inode;

  if (get_node_by_path(path, 0, &file_inode) != 0) {
    return -ENOENT;
  }

  if (offset >= file_inode.size)
    return 0;

  if (offset + size > file_inode.size) {
    size = file_inode.size - offset;
  }
  // Step 2: Based on size and offset, read its data blocks from disk
  size_t bytes_read = 0;
  char *block_buf = (char *)malloc(BLOCK_SIZE);

  // Step 3: copy the correct amount of data from offset to buffer
  while (bytes_read < size) {
    /* Calculate which block and offset within block */
    int block_idx = (offset + bytes_read) / BLOCK_SIZE;
    int block_offset = (offset + bytes_read) % BLOCK_SIZE;

    /* Check if we have a valid block pointer */
    if (block_idx >= 16 || file_inode.direct_ptr[block_idx] == -1)
      break;

    bio_read(sb.d_start_blk + file_inode.direct_ptr[block_idx], block_buf);

    size_t bytes_to_copy = BLOCK_SIZE - block_offset;
    if (bytes_to_copy > size - bytes_read) {
      bytes_to_copy = size - bytes_read;
    }

    /* Copy data to output bufer */
    memcpy(buffer + bytes_read, block_buf + block_offset, bytes_to_copy);
    bytes_read += bytes_to_copy;
  }
  // Note: this function should return the amount of bytes you copied to buffer
  time(&file_inode.vstat.st_atime);
  writei(file_inode.ino, &file_inode);
  free(block_buf);
  return bytes_read;
}

static int rufs_write(const char *path, const char *buffer, size_t size,
                      off_t offset, struct fuse_file_info *fi) {

  // Step 1: You could call get_node_by_path() to get inode from path
  struct inode file_inode;
  if (get_node_by_path(path, 0, &file_inode) != 0)
    return -ENOENT;
  // Step 2: Based on size and offset, read its data blocks from disk
  size_t bytes_written = 0;
  char *block_buf = (char *)malloc(BLOCK_SIZE);
  // Step 3: Write the correct amount of data from offset to disk
  while (bytes_written < size) {
    // Calculate which block and offset within block
    int block_idx = (offset + bytes_written) / BLOCK_SIZE;
    int block_offset = (offset + bytes_written) % BLOCK_SIZE;

    // Check if we're within direct pointer limits
    if (block_idx >= 16) {
      // Can't support files this large without indirect pointers
      break;
    }

    // Allocate a new block if needed
    if (file_inode.direct_ptr[block_idx] == -1) {
      int new_blkno = get_avail_blkno();
      if (new_blkno == -1) {
        // No more space
        break;
      }
      file_inode.direct_ptr[block_idx] = new_blkno;

      // Initialize the new block to zeros
      memset(block_buf, 0, BLOCK_SIZE);
    } else {
      // Read existing block (for partial block writes)
      bio_read(sb.d_start_blk + file_inode.direct_ptr[block_idx], block_buf);
    }

    // Calculate how many bytes to write to this block
    size_t bytes_to_write = BLOCK_SIZE - block_offset;
    if (bytes_to_write > size - bytes_written) {
      bytes_to_write = size - bytes_written;
    }

    // Copy data from input buffer to block buffer
    memcpy(block_buf + block_offset, buffer + bytes_written, bytes_to_write);

    // Write the block back to disk
    bio_write(sb.d_start_blk + file_inode.direct_ptr[block_idx], block_buf);

    bytes_written += bytes_to_write;
  }
  // Step 4: Update the inode info and write it to disk
  // Update file size if we wrote beyond the current end
  if (offset + bytes_written > file_inode.size) {
    file_inode.size = offset + bytes_written;
    file_inode.vstat.st_size = file_inode.size;
  }

  // Update modification time
  time(&file_inode.vstat.st_mtime);
  writei(file_inode.ino, &file_inode);

  free(block_buf);
  return bytes_written;
}

/*
 * Functions you DO NOT need to implement for this project
 * (stubs provided for completeness)
 */

static int rufs_rmdir(const char *path) {
  // For this project, you don't need to fill this function
  // But DO NOT DELETE IT!
  return 0;
}

static int rufs_releasedir(const char *path, struct fuse_file_info *fi) {
  // For this project, you don't need to fill this function
  // But DO NOT DELETE IT!
  return 0;
}

static int rufs_unlink(const char *path) {
  // For this project, you don't need to fill this function
  // But DO NOT DELETE IT!
  return 0;
}

static int rufs_truncate(const char *path, off_t size) {
  // For this project, you don't need to fill this function
  // But DO NOT DELETE IT!
  return 0;
}

static int rufs_release(const char *path, struct fuse_file_info *fi) {
  // For this project, you don't need to fill this function
  // But DO NOT DELETE IT!
  return 0;
}

static int rufs_flush(const char *path, struct fuse_file_info *fi) {
  // For this project, you don't need to fill this function
  // But DO NOT DELETE IT!
  return 0;
}

static int rufs_utimens(const char *path, const struct timespec tv[2]) {
  // For this project, you don't need to fill this function
  // But DO NOT DELETE IT!
  return 0;
}

static struct fuse_operations rufs_ope = {
    .init = rufs_init,
    .destroy = rufs_destroy,

    .getattr = rufs_getattr,
    .readdir = rufs_readdir,
    .opendir = rufs_opendir,
    .mkdir = rufs_mkdir,

    .create = rufs_create,
    .open = rufs_open,
    .read = rufs_read,
    .write = rufs_write,

    // Operations that you don't have to implement.
    .rmdir = rufs_rmdir,
    .releasedir = rufs_releasedir,
    .unlink = rufs_unlink,
    .truncate = rufs_truncate,
    .flush = rufs_flush,
    .utimens = rufs_utimens,
    .release = rufs_release};

int main(int argc, char *argv[]) {
  int fuse_stat;

  getcwd(diskfile_path, PATH_MAX);
  strcat(diskfile_path, "/DISKFILE");

  fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

  return fuse_stat;
}
