// Author: Jordan Miller, McGill University
// April 11, 2017
// memory leaks are a result of the test suite not freeing malloc'd memory, not this!
// Notes: I had it working perfectly on my mac, and after moving it to Trottier it
// started failing a lot. I tried using clang and GCC on both machines, same results...
// I managed to get it working, but also found when I was debugging that things would
// work better in GCC than clang.
#include "sfs_api.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define FSNAME "testsys"

int init_fresh_disk(char *filename, int block_size, int num_blocks);
int init_disk(char *filename, int block_size, int num_blocks);
int read_blocks(int start_address, int nblocks, void *buffer);
int write_blocks(int start_address, int nblocks, void *buffer);
int update_root_directory(int, char*);
int ssfs_fwrite_no_allocate(int, char*, int);
int get_empty_block();
int close_disk();

// useful for assembling reads/writes
// must be packed in order that padding doesn't cause data to be lost
struct __attribute__((__packed__)) block {
  unsigned char bytes[1024];
} block_t;

// 1 inode per file, 64 bytes each (16 inodes per block)
// must be packed in order that padding doesn't cause data to be lost
struct __attribute__((__packed__)) inode {
  int size;
  int direct[14];
  int indirect;
} inode_t;

// stored at beginning of file system; takes one block
// must be packed in order that padding doesn't cause data to be lost
struct __attribute__((__packed__)) superblock {
  int magic_number;
  int super_block_size;
  int super_num_blocks;
  struct inode jnode;
} superblock_t;

struct fd {
  struct inode descriptor_inode;
  int fd_inode_index;
  int read_ptr;
  int write_ptr;
  int written;
} fd_t;

static struct superblock super;
static struct block fbm;
char root_directory[200][16];
static struct fd file_descriptor_table[32];
int fd_counter = 0; // counter for file descriptors

void mkssfs(int fresh){

  // upon creation/loading of fs, all fd's must be replaced/reset
  fd_counter = 0;
  for (int i = 0; i < 32; i++) {
    file_descriptor_table[i].fd_inode_index = 0;
    file_descriptor_table[i].read_ptr = 0;
    file_descriptor_table[i].write_ptr = 0;
    file_descriptor_table[i].written = 0;
  }
  if (fresh == 1) { // if new file system requested

    // initializing super block- also stored in memory
    init_fresh_disk(FSNAME, 1024, 1024);
    super.magic_number = 0xACBD0005;
    super.super_block_size = 1024;
    super.super_num_blocks = 1024;
    struct inode new_fs_jnode;
    new_fs_jnode.size = 12800;
    for (int i = 0; i < 14; i++) {
      new_fs_jnode.direct[i] = i + 1;
    }
    new_fs_jnode.indirect = 0;
    super.jnode = new_fs_jnode;
    write_blocks(0, 1, &super);

    // initialize and store first inode
    struct inode root_dir_inode;
    root_dir_inode.size = 3200;
    for (int i = 0; i < 4; i++) { // stores pointers to root dir
      root_dir_inode.direct[i] = i+14;
    }
    root_dir_inode.indirect = 0;
    write_blocks(1, 1, &root_dir_inode);

    //storing root directory in 0
    update_root_directory(0, "root");

    // initializing FBM
    // 0th block is super, 1st-13th are inodes, 14-17th are root dir, 1023rd is fbm
    // these blocks are marked as used to reserve them
    for (int i = 18; i < 1023; i++) {
      fbm.bytes[i] = 1;
    }
    write_blocks(1023, 1, &fbm);

  } else { // if old file system used

    init_disk(FSNAME, 1024, 1024);
    read_blocks(0, 1, &super); // initialize super block; store in memory
    if (super.magic_number != 0xACBD0005) {
      printf("Magic Number incorrect- wrong file system\n");
    }
    read_blocks(1023, 1, &fbm); // store fbm in mem
    read_blocks(14, 4, &root_directory); // store root directory in mem

  }
}

// compares root directory index with file names
// returns inode index on success or -1 on failure
int get_inode_from_name(char* name) {
  read_blocks(14, 4, &root_directory);
  for (int i = 0; i < 200; i++) {
    if (root_directory[i] != NULL && strcmp(name, root_directory[i]) == 0) {
      return i;
    }
  }
  return -1;
}

// gets index of the first inode that can be replaced
// returns -1 on failure
int get_null_inode() {
  read_blocks(14, 4, &root_directory);
  for (int i = 0; i < 200; i++) {
    if (root_directory[i][0] == 0) {
      return i;
    }
  }
  return -1;
}

// gets index of first fd that can be replaced/filled
// returns -1 on failure
int get_empty_fd() {
  for (int i = 0; i < 32; i++) {
    if (file_descriptor_table[i].written == 0) {
      file_descriptor_table[i].fd_inode_index = 0;
      file_descriptor_table[i].read_ptr = 0;
      file_descriptor_table[i].write_ptr = 0;
      fd_counter++;
      return i;
    }
  }
  return -1;
}

// updates root directory given name/inode index and name
// returns -1 on failure, 0 on success
int update_root_directory(int file_index, char *name) {
  read_blocks(14, 4, &root_directory);
  if (read_blocks(14, 4, &root_directory) < 0) {
    return -1;
  }
  if (strncpy(root_directory[file_index], name, 16) < 0) { // if copy fails
    return -1;
  }
  if (write_blocks(14, 4, &root_directory) < 0) {
    return -1;
  }
  return 0;
}

// gets index of empty block to write to, zeroes the block, and marks as occupied in fbm
// returns -1 on failure
int get_empty_block() {
  int fresh_block_index = -1;
  for (int i = 18; i < 1023; i++) { // determine first free block
    if (fbm.bytes[i] == 1) {
      fresh_block_index = i;
      fbm.bytes[i] = 0;
      write_blocks(i, 1, &fbm);
      break;
    }
  }
  if (fresh_block_index != -1) { // zeroing new block
    struct block fresh_block;
    for (int i = 0; i < 1024; i++) {
      fresh_block.bytes[i] = 0;
    }
    write_blocks(fresh_block_index, 1, &fresh_block);
  }
  return fresh_block_index;
}

// opens new fd in file_descriptor_table
// returns fd index on success, -1 on failure
int ssfs_fopen(char *name){
  int inode_index = get_inode_from_name(name); // gets index of inode associated with name through root dir
  int fd_index = get_empty_fd();
  if (fd_index < 0) {
    printf("No space for additional file descriptors- there are %i fds open\n", fd_counter);
    return -1;
  }

  if (inode_index < 0) { // if name is not matched, new file is needed
    inode_index = get_null_inode(); // gets first inode for file
    if (inode_index < 0) {
      printf("No space for additional inodes\n");
      return -1;
    }

    // insert name into root directory
    if (update_root_directory(inode_index, name) < 0) {
      printf("Updating root directory failed\n");
      return -1;
    }

    struct inode new_file_inode;
    new_file_inode.size = 0;
    for (int i = 0; i < 14; i++) { // old data fields require zeroing in case of reuse
      new_file_inode.direct[i] = 0;
    }
    new_file_inode.indirect = 0;

    // store new inode
    struct inode old_inode_block[16];
    read_blocks(inode_index/16 + 1, 1, &old_inode_block);
    old_inode_block[inode_index%16] = new_file_inode;
    write_blocks(inode_index/16 + 1, 1, &old_inode_block);

    // make new fd in append mode
    file_descriptor_table[fd_index].descriptor_inode = new_file_inode;
    file_descriptor_table[fd_index].fd_inode_index = inode_index;
    file_descriptor_table[fd_index].read_ptr = 0;
    file_descriptor_table[fd_index].write_ptr = 0;
    file_descriptor_table[fd_index].written = 1;
    return fd_index;
  } else { // make new fd in append mode

    // get file's inode
    struct inode new_file_inode;
    struct inode old_inode_block[16];
    read_blocks(inode_index/16 + 1, 1, &old_inode_block);
    new_file_inode = old_inode_block[inode_index%16];

    // initialize fd, store in mem
    struct fd new_file_fd;
    new_file_fd.descriptor_inode = new_file_inode;
    new_file_fd.fd_inode_index = inode_index;
    new_file_fd.read_ptr = 0;
    new_file_fd.write_ptr = new_file_inode.size;
    new_file_fd.written = 1;
    file_descriptor_table[fd_index] = new_file_fd;

    return fd_index;
  }
}

// closes fd in file_descriptor_table, invalid-pointer-safe
// returns 0 on success, -1 on failure
int ssfs_fclose_index(int fileID) {
    if (file_descriptor_table[fileID].written == 1) {
      file_descriptor_table[fileID].written = 0;
    } else {
      return -1;
    }
    return 0;
}

// closes ALL references to file, despite different indices in fdt
// returns 0 on success, -1 on failure
int ssfs_fclose(int fileID) {
  if (fileID < 0 || fileID > 31) { // invalid fileID
    return -1;
  }
  int inode_index = file_descriptor_table[fileID].fd_inode_index;
  int returner = -1; // value to return- if not updated, will return -1
  for (int i = 0; i < 32; i++) {
    if (file_descriptor_table[i].fd_inode_index == inode_index) { // if fdt[i] points to the same file as fdt[fileID]
      if (ssfs_fclose_index(i) != -1) { // if able to close a file
        returner = 0;
        fd_counter--;
      }
    }
  }
  return returner;
}

// moves read pointer to new location, if in range
// if new location is beyond file size, moves to end of file
// returns 0 on success, -1 on failure
int ssfs_frseek(int fileID, int loc) {

  struct inode inode_block_to_read_from[16]; // making sure inode is current
  read_blocks(file_descriptor_table[fileID].fd_inode_index/16 + 1, 1, &inode_block_to_read_from);
  file_descriptor_table[fileID].descriptor_inode = inode_block_to_read_from[file_descriptor_table[fileID].fd_inode_index%16];

    if (loc < 0 || fileID < 0 || fileID > 31 || file_descriptor_table[fileID].written == 0) {
      // invalid seek
      return -1;
    }

    else {
      file_descriptor_table[fileID].read_ptr = loc;
      if (file_descriptor_table[fileID].descriptor_inode.size < loc) {
        // if attempting to seek beyond end of file
        file_descriptor_table[fileID].read_ptr = file_descriptor_table[fileID].descriptor_inode.size;
      }

      return 0;
    }

}

// moves read pointer to new location, if in range
// if new location is beyond file size, moves to end of file
// returns 0 on success, -1 on failure
int ssfs_fwseek(int fileID, int loc){

  struct inode inode_block_to_read_from[16]; // making sure inode is current
  read_blocks(file_descriptor_table[fileID].fd_inode_index/16 + 1, 1, &inode_block_to_read_from);
  file_descriptor_table[fileID].descriptor_inode = inode_block_to_read_from[file_descriptor_table[fileID].fd_inode_index%16];

  if (loc < 0 || fileID < 0 || fileID > 31 || file_descriptor_table[fileID].written == 0) {
    // invalid seek
    return -1;
  }

  else {
    file_descriptor_table[fileID].write_ptr = loc;
    if (file_descriptor_table[fileID].descriptor_inode.size < loc) {
      // if attempting to seek beyond end of file
      file_descriptor_table[fileID].write_ptr = file_descriptor_table[fileID].descriptor_inode.size;
    }
    return 0;
  }
}

// checks if this is the right inode to write to
// if not, it recurses until it is
// if it is, will write until inode blocks are full and then recurse to write to the next inode
void ssfs_recursive_write(int starting_position, int inode_index, int buffer_index, char* buf, int length) {
  if (starting_position > 14335) { // recursing to the next inode
    struct inode inode_to_write;
    struct inode old_inode_block[16];
    read_blocks(inode_index/16 + 1, 1, &old_inode_block);
    inode_to_write = old_inode_block[inode_index%16];
    ssfs_recursive_write(starting_position - 14336, inode_to_write.indirect, buffer_index, buf, length);
    return;
  } else {

    // this is the right inode to write to
    // get block of inodes to write to
    struct inode inode_to_write;
    struct inode old_inode_block[16];
    read_blocks(inode_index/16 + 1, 1, &old_inode_block);
    inode_to_write = old_inode_block[inode_index%16];

    int local_write_pointer = starting_position%1024; // position in block to write to

    for (int i = starting_position/1024; i < 14 && buffer_index < length; i++) {
      struct block write_to; // block to write to
      read_blocks(inode_to_write.direct[i], 1, &write_to);

      while (buffer_index < length && local_write_pointer < 1024) { // copying bytes to new block
        memcpy(write_to.bytes+local_write_pointer, buf+buffer_index, 1);
        local_write_pointer++;
        buffer_index++;
      }
      write_blocks(inode_to_write.direct[i], 1, &write_to);
      local_write_pointer = 0; // means next block starts from 0
    }
    if (buffer_index < length) { // if there's more to write, recursively calling next inode to write to
      ssfs_recursive_write(0, inode_to_write.indirect, buffer_index, buf, length);
      return;
    }
    return;
  }
}

// based on number of blocks, recursively allocates new inodes if necessary
// returns 0 on success, -1 on failure
// process:
// 1. check if this is the right inode to be writing to
// 2. assign blocks from new_blocks using blocks_before_write
// 3. if more pointers needed, allocate new inode and recursively call
int ssfs_recursive_allocate(int new_blocks[], int new_block_index, int num_blocks, int size, int blocks_before_write, int inode_index) {

  struct inode inode_block[16];
  read_blocks(inode_index/16 + 1, 1, &inode_block);
  inode_block[inode_index%16].size = size;
  write_blocks(inode_index/16 + 1, 1, &inode_block); // avoiding having more than 1 inode open across a recursive call
  if (blocks_before_write > 14) { // passes through "chain" of inodes until appropriate
    return ssfs_recursive_allocate(new_blocks, new_block_index, num_blocks, size, blocks_before_write - 14, inode_block[inode_index%16].indirect);
  }
  read_blocks(inode_index/16 + 1, 1, &inode_block);
  // links blocks into inode
  for (int i = blocks_before_write; i < 14 && new_block_index < num_blocks; i++) {
    inode_block[inode_index%16].direct[i] = new_blocks[new_block_index++];
  }

  // "saving" inode to disc
  inode_block[inode_index%16].size = size;
  int new_inode_index;
  if (new_block_index < num_blocks) { // if new inode is needed
    new_inode_index = get_null_inode();
    inode_block[inode_index%16].indirect = new_inode_index;
  }
  write_blocks(inode_index/16 + 1, 1, &inode_block);

  // if new inode needed, creating new inode and then recursing into it
  if (new_block_index < num_blocks) {

    if (new_inode_index < 0) { // if failure getting new inode
      return -1;
    }

    read_blocks(new_inode_index/16 + 1, 1, &inode_block);

    inode_block[new_inode_index%16].size = size;
    for (int i = 0; i < 14; i++) {
      inode_block[new_inode_index%16].direct[i] = 0;
    }

    write_blocks(new_inode_index/16 + 1, 1, &inode_block);
    update_root_directory(new_inode_index, "(anonIN)");

    return ssfs_recursive_allocate(new_blocks, new_block_index, num_blocks, size, 0, new_inode_index);

  }
  return 0;
}

// first determines new size, then allocates more blocks until size requirement is met
// then calls recursive_allocate to put those blocks into inodes
// then recursive_writes into those blocks
// moves write pointer to byte past end of write
// returns size of write on success, or -1 on failure
int ssfs_fwrite(int fileID, char *buf, int length) {

  if (length < 0) {
    return -1;
  }

  struct inode inode_block_to_read_from[16]; // making sure inode is current
  read_blocks(file_descriptor_table[fileID].fd_inode_index/16 + 1, 1, &inode_block_to_read_from);
  file_descriptor_table[fileID].descriptor_inode = inode_block_to_read_from[file_descriptor_table[fileID].fd_inode_index%16];
  int new_size = file_descriptor_table[fileID].descriptor_inode.size - (file_descriptor_table[fileID].descriptor_inode.size - file_descriptor_table[fileID].write_ptr) + length;

  if (new_size < file_descriptor_table[fileID].descriptor_inode.size) { // writing can never make a file size smaller...
    new_size = file_descriptor_table[fileID].descriptor_inode.size;
  }

  int current_no_of_blocks = file_descriptor_table[fileID].descriptor_inode.size/1024;
  if (file_descriptor_table[fileID].descriptor_inode.size%1024 > 0) current_no_of_blocks++;
  int blocks_needed = new_size/1024;
  if(new_size%1024 > 0) blocks_needed++;
  int blocks_to_allocate = blocks_needed - current_no_of_blocks;

  if (blocks_to_allocate > 0) { // allocates new memory
    int *new_blocks;
    new_blocks = malloc(blocks_to_allocate * 4);

    for (int i = 0; i < blocks_to_allocate; i++) {
      new_blocks[i] = get_empty_block();
      if (new_blocks[i] < 0) { // failure to get a new block == bad write
        for (int j = 0; j < i; j++) { // deallocating blocks that were just allocated
          fbm.bytes[new_blocks[j]] = 1;
        }
        write_blocks(1023, 1, &fbm);
        printf("block allocation fail\n");
        free(new_blocks);
        return -1;
      }
    }

    // putting new blocks into inodes
    if (ssfs_recursive_allocate(new_blocks, 0, blocks_to_allocate, new_size, current_no_of_blocks, file_descriptor_table[fileID].fd_inode_index) < 0) {
      printf("allocation fail\n");
      return -1;
    }
    struct inode inode_block[16];
    read_blocks(file_descriptor_table[fileID].fd_inode_index/16 + 1, 1, &inode_block);
    file_descriptor_table[fileID].descriptor_inode = inode_block[file_descriptor_table[fileID].fd_inode_index%16];
    write_blocks(file_descriptor_table[fileID].fd_inode_index/16 + 1, 1, &inode_block);
    free(new_blocks);
  }

  // writes to blocks/inodes, relying on the above to allocate enough memory
  ssfs_recursive_write(file_descriptor_table[fileID].write_ptr, file_descriptor_table[fileID].fd_inode_index, 0, buf, length);

  // updating size in first file inode and fdt
  struct inode fd_inode_block[16];
  read_blocks(file_descriptor_table[fileID].fd_inode_index/16 + 1, 1, &fd_inode_block);
  fd_inode_block[file_descriptor_table[fileID].fd_inode_index%16].size = new_size;

  file_descriptor_table[fileID].descriptor_inode = fd_inode_block[file_descriptor_table[fileID].fd_inode_index%16];
  write_blocks(file_descriptor_table[fileID].fd_inode_index/16 + 1, 1, &fd_inode_block);
  file_descriptor_table[fileID].write_ptr += length;

  return length;
}

// returns bytes read on the heap.
// process: if read doesn't start in this inode, recurses until it does
// else, allocates a builder on the heap, loads correct blocks, and copies bytes into it
// if read continues into next inode, it will copy
// the result of a recursive call on the next inode and then free. NB: I'm proud of this one!
char* ssfs_recursive_read(int starting_position, int left_to_read, int inode_index) {
  if (starting_position > 14335) { // if this inode doesn't point to the first byte to read
    struct inode inode_to_read;
    struct inode old_inode_block[16];
    read_blocks(inode_index/16 + 1, 1, &old_inode_block);
    inode_to_read = old_inode_block[inode_index%16];
    return ssfs_recursive_read(starting_position - 14336, left_to_read, inode_to_read.indirect);

  } else { // if this inode contains bytes to read
    // initializing inode to read from
    struct inode inode_to_read;
    struct inode old_inode_block[16];
    read_blocks(inode_index/16 + 1, 1, &old_inode_block);
    inode_to_read = old_inode_block[inode_index%16];

    char* builder = malloc(left_to_read); // string to return
    int builder_write_pointer = 0; // index in builder to write to
    int local_read_pointer = starting_position%1024; // position in block to read from
    for (int i = starting_position/1024; i < 14 && left_to_read > 0; i++) {
      struct block read_from; // block to read from
      read_blocks(inode_to_read.direct[i], 1, &read_from);
      while (left_to_read > 0 && local_read_pointer < 1024) { // copying bytes
        builder[builder_write_pointer++] = read_from.bytes[local_read_pointer++];
        left_to_read--;
      }
      local_read_pointer = 0; // means next block starts from 0
    }
    if (left_to_read > 0) { // recursively calling next inode
      char* rec_result = ssfs_recursive_read(0, left_to_read, inode_to_read.indirect);
      memcpy(builder+builder_write_pointer, rec_result, left_to_read);
      free(rec_result);
    } // builder contains all the requested data
    return builder;
  }
}

// will read from read pointer into buffer for length of read using recursive_read
// will not read beyond EOF, but if a longer read is requested, will truncate
// moves the read pointer to point to byte past end of read
// returns length of read on success, -1 on failure
int ssfs_fread(int fileID, char *buf, int length){

  struct inode inode_block_to_read_from[16]; // making sure inode is current
  read_blocks(file_descriptor_table[fileID].fd_inode_index/16 + 1, 1, &inode_block_to_read_from);
  file_descriptor_table[fileID].descriptor_inode = inode_block_to_read_from[file_descriptor_table[fileID].fd_inode_index%16];

  // check for valid read
  if (fileID < 0 || fileID > 31 || file_descriptor_table[fileID].written == 0 || length < 0) {
    return -1;
  }

  // if attempting to read beyond EOF, truncating
  if (length + file_descriptor_table[fileID].read_ptr > file_descriptor_table[fileID].descriptor_inode.size) {
    length = file_descriptor_table[fileID].descriptor_inode.size - file_descriptor_table[fileID].read_ptr;
  }

  // recursive_read returns string on heap
  char* rec_result = ssfs_recursive_read(file_descriptor_table[fileID].read_ptr, length, file_descriptor_table[fileID].fd_inode_index);
  strncpy(buf, rec_result, length); // copies into buffer, frees allocated mem
  free(rec_result);
  file_descriptor_table[fileID].read_ptr += length;
  return length;
}

// removes and zeroes inodes, then recursively calls on the indirect inode it points to if necessary
int remove_inode(int dir_index) {
  struct inode inode_to_read;
  struct inode old_inode_block[16];
  read_blocks(dir_index/16 + 1, 1, &old_inode_block);
  inode_to_read = old_inode_block[dir_index%16];
  int linked_inode = inode_to_read.indirect;

  for (int i = 0; i < 14; i++) { // updating fbm
    fbm.bytes[inode_to_read.direct[i]] = 1;
  }
  write_blocks(1023, 1, &fbm);

  memset(&old_inode_block[dir_index%16], 0, 64); // zeroing inode
  write_blocks(dir_index/16 + 1, 1, &old_inode_block);

  char *nuller = "\0";
  update_root_directory(dir_index, nuller);

  if (linked_inode > 0) { // recursively updating indirect inodes
    return remove_inode(linked_inode);
  }
  return 0;
}

// removes all files with the same name from the directory
// returns 0 on success, -1 on failure
int ssfs_remove(char *file) {
  for (int i = 0; i < 200; i++) { // attempts to remove all files of the same name

    int inode_to_remove = get_inode_from_name(file);
    if (inode_to_remove < 0) return 0;
    if (inode_to_remove == 0) { // if file is not found
      return -1;
    }

    for (int i = 0; i < 32; i++) { // must remove from fdt
      struct inode inode_block_to_read_from[16]; // making sure inode is current
      read_blocks(file_descriptor_table[i].fd_inode_index/16 + 1, 1, &inode_block_to_read_from);
      file_descriptor_table[i].descriptor_inode = inode_block_to_read_from[file_descriptor_table[i].fd_inode_index%16];
      if (file_descriptor_table[i].fd_inode_index == inode_to_remove) {
        if (ssfs_fclose(i) < 0) {
          file_descriptor_table[i].written = 0;
        }
        break;
      }
    }

    if (remove_inode(inode_to_remove) < 0) {
      return -1;
    }
  }
  return -1;
}
