/*
 * Interface functions between file system and kernel/processes. added @lab4_1
 */

#include "proc_file.h"

#include "hostfs.h"
#include "pmm.h"
#include "process.h"
#include "ramdev.h"
#include "rfs.h"
#include "riscv.h"
#include "spike_interface/spike_file.h"
#include "spike_interface/spike_utils.h"
#include "util/functions.h"
#include "util/string.h"

//
// initialize file system
//
void fs_init(void) {
  // initialize the vfs
  vfs_init();

  // register hostfs and mount it as the root
  if( register_hostfs() < 0 ) panic( "fs_init: cannot register hostfs.\n" );
  struct device *hostdev = init_host_device("HOSTDEV");
  vfs_mount("HOSTDEV", MOUNT_AS_ROOT);

  // register and mount rfs
  if( register_rfs() < 0 ) panic( "fs_init: cannot register rfs.\n" );
  struct device *ramdisk0 = init_rfs_device("RAMDISK0");
  rfs_format_dev(ramdisk0);
  vfs_mount("RAMDISK0", MOUNT_DEFAULT);
}

//
// initialize a proc_file_management data structure for a process.
// return the pointer to the page containing the data structure.
//
proc_file_management *init_proc_file_management(void) {
  proc_file_management *pfiles = (proc_file_management *)alloc_page();
  pfiles->cwd = vfs_root_dentry; // by default, cwd is the root
  pfiles->nfiles = 0;

  for (int fd = 0; fd < MAX_FILES; ++fd)
    pfiles->opened_files[fd].status = FD_NONE;

  sprint("FS: created a file management struct for a process.\n");
  return pfiles;
}

//
// reclaim the open-file management data structure of a process.
// note: this function is not used as PKE does not actually reclaim a process.
//
void reclaim_proc_file_management(proc_file_management *pfiles) {
  free_page(pfiles);
  return;
}

//
// get an opened file from proc->opened_file array.
// return: the pointer to the opened file structure.
//
struct file *get_opened_file(int fd) {
  struct file *pfile = NULL;

  // browse opened file list to locate the fd
  for (int i = 0; i < MAX_FILES; ++i) {
    pfile = &(current->pfiles->opened_files[i]);  // file entry
    if (i == fd) break;
  }
  if (pfile == NULL) panic("do_read: invalid fd!\n");
  return pfile;
}

//
// open a file named as "pathname" with the permission of "flags".
// return: -1 on failure; non-zero file-descriptor on success.
//
int do_open(char *pathname, int flags) {
  struct file *opened_file = NULL;
  // convert relative path to absolute path
  char abs_path[MAX_PATH_LEN];
  if (pathname[0] == '.'){
    if (pathname[1] == '/'){
      do_rcwd(abs_path);
      if (abs_path[strlen(abs_path)-1] != '/')
        strcat(abs_path, "/");
      strcat(abs_path, pathname+2);
    }else if (pathname[1] == '.'){
      char temp_path[MAX_PATH_LEN];
      do_rcwd(temp_path);
      int last_slash = -1;
      for (int i = strlen(temp_path) - 1; i >= 0; --i) {
        if (temp_path[i] == '/') {
          last_slash = i;
          break;
        }
      }
      if (last_slash != -1){
        temp_path[last_slash] = '\0'; // remove the last part
      }else{
        strcpy(temp_path, "/");
      }
      strcpy(abs_path, temp_path);
      if (abs_path[strlen(abs_path)-1] != '/')
        strcat(abs_path, "/");
      strcat(abs_path, pathname+3);
    }else{
      do_rcwd(abs_path);
      if (abs_path[strlen(abs_path)-1] != '/')
        strcat(abs_path, "/");
      strcat(abs_path, pathname);
    }
  }else if (pathname[0] == '/'){
    strcpy(abs_path, pathname);
  }else{
    do_rcwd(abs_path);
    if (abs_path[strlen(abs_path)-1] != '/')
      strcat(abs_path, "/");
    strcat(abs_path, pathname);
  }

  if ((opened_file = vfs_open(abs_path, flags)) == NULL) return -1;

  int fd = 0;
  if (current->pfiles->nfiles >= MAX_FILES) {
    panic("do_open: no file entry for current process!\n");
  }
  struct file *pfile;
  for (fd = 0; fd < MAX_FILES; ++fd) {
    pfile = &(current->pfiles->opened_files[fd]);
    if (pfile->status == FD_NONE) break;
  }

  // initialize this file structure
  memcpy(pfile, opened_file, sizeof(struct file));

  ++current->pfiles->nfiles;
  return fd;
}

//
// read content of a file ("fd") into "buf" for "count".
// return: actual length of data read from the file.
//
int do_read(int fd, char *buf, uint64 count) {
  struct file *pfile = get_opened_file(fd);

  if (pfile->readable == 0) panic("do_read: no readable file!\n");

  char buffer[count + 1];
  int len = vfs_read(pfile, buffer, count);
  buffer[count] = '\0';
  strcpy(buf, buffer);
  return len;
}

//
// write content ("buf") whose length is "count" to a file "fd".
// return: actual length of data written to the file.
//
int do_write(int fd, char *buf, uint64 count) {
  struct file *pfile = get_opened_file(fd);

  if (pfile->writable == 0) panic("do_write: cannot write file!\n");

  int len = vfs_write(pfile, buf, count);
  return len;
}

//
// reposition the file offset
//
int do_lseek(int fd, int offset, int whence) {
  struct file *pfile = get_opened_file(fd);
  return vfs_lseek(pfile, offset, whence);
}

//
// read the vinode information
//
int do_stat(int fd, struct istat *istat) {
  struct file *pfile = get_opened_file(fd);
  return vfs_stat(pfile, istat);
}

//
// read the inode information on the disk
//
int do_disk_stat(int fd, struct istat *istat) {
  struct file *pfile = get_opened_file(fd);
  return vfs_disk_stat(pfile, istat);
}

//
// close a file
//
int do_close(int fd) {
  struct file *pfile = get_opened_file(fd);
  return vfs_close(pfile);
}

//
// open a directory
// return: the fd of the directory file
//
int do_opendir(char *pathname) {
  struct file *opened_file = NULL;
  if ((opened_file = vfs_opendir(pathname)) == NULL) return -1;

  int fd = 0;
  struct file *pfile;
  for (fd = 0; fd < MAX_FILES; ++fd) {
    pfile = &(current->pfiles->opened_files[fd]);
    if (pfile->status == FD_NONE) break;
  }
  if (pfile->status != FD_NONE)  // no free entry
    panic("do_opendir: no file entry for current process!\n");

  // initialize this file structure
  memcpy(pfile, opened_file, sizeof(struct file));

  ++current->pfiles->nfiles;
  return fd;
}

//
// read a directory entry
//
int do_readdir(int fd, struct dir *dir) {
  struct file *pfile = get_opened_file(fd);
  return vfs_readdir(pfile, dir);
}

//
// make a new directory
//
int do_mkdir(char *pathname) {
  return vfs_mkdir(pathname);
}

//
// close a directory
//
int do_closedir(int fd) {
  struct file *pfile = get_opened_file(fd);
  return vfs_closedir(pfile);
}

//
// create hard link to a file
//
int do_link(char *oldpath, char *newpath) {
  return vfs_link(oldpath, newpath);
}

//
// remove a hard link to a file
//
int do_unlink(char *path) {
  return vfs_unlink(path);
}

//
// read current working directory
//
int do_rcwd(char *cwd){
  memset(cwd, 0, MAX_PATH_LEN);
  struct dentry *cwd_entry = current->pfiles->cwd;
  char path[16][MAX_PATH_LEN];
  
  int i = 0;
  for (; cwd_entry != NULL && cwd_entry != vfs_root_dentry; i++){
    strcpy(path[i], cwd_entry->name);
    cwd_entry = cwd_entry->parent;
  }
  strcat(cwd, "/");
  while (--i>=0){
    strcat(cwd, path[i]);
    if (i!=0)
      strcat(cwd, "/");
  }

  return 0;
}


//
// change current working directory
//
int do_ccwd(char *path) {
  char miss_name[MAX_PATH_LEN];
  struct dentry *next_dentry = NULL;
  
  if (path[0]=='.'){
    if (path[1]=='.'){ 
      if(path[2]=='/'){ // go to parent directory
        struct dentry *parent_dentry = current->pfiles->cwd->parent;
        if (parent_dentry != NULL)
          current->pfiles->cwd = parent_dentry;
        return do_ccwd(path+3);
      }else if(path[2]=='\0'){
        struct dentry *parent_dentry = current->pfiles->cwd->parent;
        if (parent_dentry != NULL)
          current->pfiles->cwd = parent_dentry;
        return 0;
      }else{
        sprint("do_ccwd: invalid path %s\n", path);
        return -1;
      }
    }else if (path[1]=='/'){ // go to sub directory
      return do_ccwd(path+2);
    }else if(path[1]=='\0'){ // just "."
      return 0;
    }else{
      sprint("do_ccwd: invalid path %s\n", path);
      return -1; 
    }
  }else if (path[0]=='/'){ // absolute path
    struct dentry *root = vfs_root_dentry;
    next_dentry = lookup_final_dentry(path, &root, miss_name);
    if (next_dentry == NULL || next_dentry->dentry_inode->type != DIR_I) {
      sprint("do_ccwd: directory %s not found\n", miss_name);
      return -1;
    }
    current->pfiles->cwd = next_dentry;
    return 0;
  }else if(path[0]=='\0'){ // stay at current directory
    return 0;
  }else{ // go to sub directory (relative path)
    next_dentry = lookup_final_dentry(path, &(current->pfiles->cwd), miss_name);
    if (next_dentry == NULL || next_dentry->dentry_inode->type != DIR_I) {
      sprint("do_ccwd: directory %s not found\n", miss_name);
      return -1;
    }
    current->pfiles->cwd = next_dentry;
    return 0;
  }
}