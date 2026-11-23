
#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include "node.h"
#include "dir.h"
#include "statistics.h"
#include "locks.h"
#include <errno.h>

#define BLOCKSIZE 4096

#define O_WRITE(flags) (((flags) &O_ACCMODE) == O_WRONLY || ((flags) & O_ACCMODE) == O_RDWR)
#define O_READ(flags)  (((flags) &O_ACCMODE) == O_RDONLY || ((flags) & O_ACCMODE) == O_RDWR)

#define U_ATIME (1 << 0)
#define U_CTIME (1 << 1)
#define U_MTIME (1 << 2)

struct filesystem {
  struct node *root;
  struct rwmutex lock;
};

struct filehandle {
  struct node *node;
  int o_flags;
};

struct filesystem the_fs;


//
// Utility functions
//

char * safe_dirname(const char *msg) {
  char *buf = strdup(msg);
  char *dir = dirname(buf);
  char *res = strdup(dir);
  free(buf);
  return res;
}

char * safe_basename(const char *msg) {
  char *buf = strdup(msg);
  char *nam = basename(buf);
  char *res = strdup(nam);
  free(buf);
  return res;
}

int getnodebypath(const char *path, struct filesystem *fs, struct node **node) {
  return getnoderelativeto(path, fs->root, node);
}

static void update_times(struct node *node, int which) {
  time_t now = time(0);
  if(which & U_ATIME) node->vstat.st_atime = now;
  if(which & U_CTIME) node->vstat.st_ctime = now;
  if(which & U_MTIME) node->vstat.st_mtime = now;
}

static int initstat(struct node *node, mode_t mode) {
  struct stat *stbuf = &node->vstat;
  memset(stbuf, 0, sizeof(struct stat));
  stbuf->st_mode  = mode;
  stbuf->st_nlink = 0;
  update_times(node, U_ATIME | U_MTIME | U_CTIME);
  return 1;
}
static void destroy_node(struct node *node){
  if (!node) return;
  if (node->data) free(node->data);
  rwmutex_destroy(&node->lock);
  free(node);
}

static int createentry(const char *path, mode_t mode, struct node **node) {
  char *dirpath = safe_dirname(path);

  // Find parent node
  struct node *dir;
  int ret = getnodebypath(dirpath, &the_fs, &dir);
  free(dirpath);
  if(!ret) {
    return -errno;
  }

  // Create new node
  *node = malloc(sizeof(struct node));
  if(!*node) {
    return -ENOMEM;
  }

  (*node)->fd_count = 0;
  (*node)->delete_on_close = 0;
  (*node)->data=NULL;
  rwmutex_init(&(*node)->lock);


  // Initialize stats
  if(!initstat(*node, mode)) {
    rwmutex_destroy(&(*node)->lock);
    free(*node);
    return -errno;
  }

  struct fuse_context *ctx = fuse_get_context();
  (*node)->vstat.st_uid = ctx->uid;
  (*node)->vstat.st_gid = ctx->gid;

  // Add to parent directory
  char *name=safe_basename(path);
  if(!dir_add_alloc(dir, name , *node, 0)) {
    free(name);
    rwmutex_destroy(&(*node)->lock);
    free(*node);
    
    return -errno;
  }
  free(name);

  return 0;
}


//
// Filesystem entry points
//

static int memfs_getattr(const char *path, struct stat *stbuf) {
  rwmutex_read_lock(&the_fs.lock);

  struct node *node;
  if(!getnodebypath(path, &the_fs, &node)) {
    rwmutex_read_unlock(&the_fs.lock);
    return -errno;
  }
  rwmutex_read_lock(&node->lock);

  stbuf->st_mode   = node->vstat.st_mode;
  stbuf->st_nlink  = node->vstat.st_nlink;
  stbuf->st_size   = node->vstat.st_size;
  stbuf->st_blocks = node->vstat.st_blocks;
  stbuf->st_uid    = node->vstat.st_uid;
  stbuf->st_gid    = node->vstat.st_gid;
  stbuf->st_mtime  = node->vstat.st_mtime;
  stbuf->st_atime  = node->vstat.st_atime;
  stbuf->st_ctime  = node->vstat.st_ctime;

  // Directories contain the implicit hardlink '.'
  if(S_ISDIR(node->vstat.st_mode)) {
    stbuf->st_nlink++;
  }
  rwmutex_read_unlock(&node->lock);
  rwmutex_read_unlock(&the_fs.lock);
  return 0;
}

static int memfs_readlink(const char *path, char *buf, size_t size) {
  rwmutex_read_lock(&the_fs.lock);

  struct node *node;
  if(!getnodebypath(path, &the_fs, &node)) {
    int err = errno;
    rwmutex_read_unlock(&the_fs.lock);
    return -err;
  }
  rwmutex_read_lock(&node->lock);
  if(!S_ISLNK(node->vstat.st_mode)) {
    rwmutex_read_unlock(&node->lock);
    rwmutex_read_unlock(&the_fs.lock);
    return -ENOLINK;
  }

  // Fuse breaks compatibility with other readlink() implementations as we cannot use the return
  // value to indicate how many bytes were written. Instead, we need to null-terminate the string,
  // unless the buffer is not large enough to hold the path. In that case, fuse will null-terminate
  // the string before passing it on.

  if(node->vstat.st_size >= size) {
    memcpy(buf, node->data, size);
  } else {
    strcpy(buf, node->data);
  }
  rwmutex_read_unlock(&node->lock);
  rwmutex_read_unlock(&the_fs.lock);

  return 0;
}

static int memfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
  rwmutex_read_lock(&the_fs.lock);

  struct node *dir;
  if(!getnodebypath(path, &the_fs, &dir)) {
    int err=errno;
    rwmutex_read_unlock(&the_fs.lock);
    return -err;
  }

  if(!S_ISDIR(dir->vstat.st_mode)) {
    rwmutex_read_unlock(&the_fs.lock);
    return -ENOTDIR;
  }
  rwmutex_read_lock(&dir->lock);
  filler(buf, ".",  &dir->vstat, 0);
  rwmutex_read_unlock(&dir->lock);
  if(dir == the_fs.root) {
    filler(buf, "..", NULL, 0);
  } else {
    char *parent_path = safe_dirname(path);
    struct node *parent;
    if (getnodebypath(parent_path, &the_fs, &parent)){
      rwmutex_read_lock(&parent->lock);
      filler(buf, "..", &parent->vstat, 0);
      rwmutex_read_unlock(&parent->lock);
    }
    free(parent_path);
    
  }

  struct direntry *entry = dir->data;
  while(entry != NULL) {
    rwmutex_read_lock(&entry->node->lock);
    int full=filler(buf,entry->name , &entry->node->vstat, 0);
    rwmutex_read_unlock(&entry->node->lock);
    if(full)
      break;
    entry = entry->next;
  }
  rwmutex_read_unlock(&the_fs.lock);

  return 0;
}

static int memfs_mknod_locked(const char *path, mode_t mode, dev_t rdev) {
  struct node *node;
  int res = createentry(path, mode, &node);
  if(res) return res;
  fprintf(stderr, "[mknod] path: %s\n", path);
  
  if(S_ISREG(mode)) {
    node->data = NULL;
    node->vstat.st_blocks = 0;
  } else {
    return -ENOSYS;
  }

  return 0;
}
static int memfs_mknod(const char *path, mode_t mode, dev_t rdev){
  rwmutex_write_lock(&the_fs.lock);
  int ret=memfs_mknod_locked(path,mode,rdev);
  rwmutex_write_unlock(&the_fs.lock);
  return ret;
}

static int memfs_mkdir(const char *path, mode_t mode) {
  rwmutex_write_lock(&the_fs.lock);
  struct node *node;
  int res = createentry(path, S_IFDIR | mode, &node);
  if(res){
    rwmutex_write_unlock(&the_fs.lock);
    return res;
  }

  // No entries
  node->data = NULL;

  rwmutex_write_unlock(&the_fs.lock);

  return 0;
}

static int memfs_unlink_locked(const char *path) {
  char *dirpath, *name;
  struct node *dir, *node;

  // Find inode
  if(!getnodebypath(path, &the_fs, &node)) {
    return -errno;
  }

  if(S_ISDIR(node->vstat.st_mode)) {
    return -EISDIR;
  }

  dirpath = safe_dirname(path);

  // Find parent inode
  if(!getnodebypath(dirpath, &the_fs, &dir)) {
    free(dirpath);
    return -errno;
  }

  free(dirpath);

  name = safe_basename(path);

  // Find directory entry in parent
  if(!dir_remove(dir, name)) {
    free(name);
    return -errno;
  }

  free(name);
  int should_free=0;
  rwmutex_write_lock(&node->lock);
  // If the link count is zero, delete the associated data
  if(node->vstat.st_nlink == 0) {
    if(node->fd_count == 0) {
      should_free=1;
    }
      // No open file descriptors, we can safely delete the node
     
     else {
      // There are open file descriptors, schedule deletion
      node->delete_on_close = 1;
    }
    rwmutex_write_unlock(&node->lock);
    if (should_free){
      destroy_node(node);
    }

  }

  return 0;
}
static int memfs_unlink(const char *path){
  rwmutex_write_lock(&the_fs.lock);
  int ret=memfs_unlink_locked(path);
  rwmutex_write_unlock(&the_fs.lock);
  return ret;
}

static int memfs_rmdir(const char *path) {
  rwmutex_write_lock(&the_fs.lock);
  char *dirpath, *name;
  struct node *dir, *node;

  // Find inode
  if(!getnodebypath(path, &the_fs, &node)) {
    int err= errno;
    rwmutex_write_unlock(&the_fs.lock);
    return -err;
  }

  if(!S_ISDIR(node->vstat.st_mode)) {
    rwmutex_write_unlock(&the_fs.lock);
    return -ENOTDIR;
  }

  // Check if directory is empty
  if(node->data != NULL) {
    rwmutex_write_unlock(&the_fs.lock);
    return -ENOTEMPTY;
  }

  dirpath = safe_dirname(path);

  // Find parent inode
  if(!getnodebypath(dirpath, &the_fs, &dir)) {
    free(dirpath);
    int err= errno;
    rwmutex_write_unlock(&the_fs.lock);
    return -err;
  }

  free(dirpath);

  name = safe_basename(path);

  // Find directory entry in parent
  if(!dir_remove(dir, name)) {
    free(name);
    int err= errno;
    rwmutex_write_unlock(&the_fs.lock);
    return -err;
  }

  free(name);

  node->data =NULL;
  destroy_node(node);

  rwmutex_write_unlock(&the_fs.lock);
  return 0;
}

static int memfs_symlink(const char *from, const char *to) {
  rwmutex_write_lock(&the_fs.lock);
  struct node *node;
  int res = createentry(to, S_IFLNK | 0766, &node);
  if(res){
    rwmutex_write_unlock(&the_fs.lock);
    return res;
  }

  node->data = strdup(from);
  node->vstat.st_size = strlen(from);

  rwmutex_write_unlock(&the_fs.lock);
  return 0;
}

// TODO: Adapt to description: https://linux.die.net/man/2/rename
static int memfs_rename(const char *from, const char *to) {
  rwmutex_write_lock(&the_fs.lock);
  char *fromdir, *fromnam, *todir, *tonam;
  struct node *node, *fromdirnode, *todirnode;

  if(!getnodebypath(from, &the_fs, &node)) {
    int err=errno;
    rwmutex_write_unlock(&the_fs.lock);
    return -err;
  }

  fromdir = safe_dirname(from);

  if(!getnodebypath(fromdir, &the_fs, &fromdirnode)) {
    free(fromdir);
    int err=errno;
    rwmutex_write_unlock(&the_fs.lock);
    return -err;
  }

  free(fromdir);

  todir = safe_dirname(to);

  if(!getnodebypath(todir, &the_fs, &todirnode)) {
    free(todir);
    int err=errno;
    rwmutex_write_unlock(&the_fs.lock);
    return -err;
  }

  free(todir);

  tonam = safe_basename(to);

  // TODO: When replacing, perform the same things as when unlinking
  if(!dir_add_alloc(todirnode, tonam, node, 1)) {
    free(tonam);
    int err=errno;
    rwmutex_write_unlock(&the_fs.lock);
    return -err;
  }

  free(tonam);

  fromnam = safe_basename(from);

  if(!dir_remove(fromdirnode, fromnam)) {
    free(fromnam);
    int err=errno;
    rwmutex_write_unlock(&the_fs.lock);
    return -err;
  }

  free(fromnam);
  rwmutex_write_lock(&node->lock);
  update_times(node,U_CTIME);
  rwmutex_write_unlock(&node->lock);

  rwmutex_write_unlock(&the_fs.lock);
  return 0;
}

static int memfs_link(const char *from, const char *to) {
  rwmutex_write_lock(&the_fs.lock);
  char *todir, *tonam;
  struct node *node, *todirnode;

  if(!getnodebypath(from, &the_fs, &node)) {
    int err= errno;
    rwmutex_write_unlock(&the_fs.lock);
    return -err;
  }

  todir = safe_dirname(to);

  if(!getnodebypath(todir, &the_fs, &todirnode)) {
    free(todir);
    int err= errno;
    rwmutex_write_unlock(&the_fs.lock);
    return -err;
  }

  free(todir);

  tonam = safe_basename(to);

  if(!dir_add_alloc(todirnode, tonam, node, 0)) {
    free(tonam);
    int err= errno;
    rwmutex_write_unlock(&the_fs.lock);
    return -err;
  }
  free(tonam);
   rwmutex_write_lock(&node->lock);
  update_times(node,U_CTIME);
  rwmutex_write_unlock(&node->lock);
  rwmutex_write_unlock(&the_fs.lock);
  

  return 0;
}

static int memfs_chmod(const char *path, mode_t mode) {
  rwmutex_read_lock(&the_fs.lock);
  struct node *node;
  if(!getnodebypath(path, &the_fs, &node)) {
    int err=errno;
    rwmutex_write_unlock(&the_fs.lock);
    return -err;
  }
  rwmutex_write_lock(&node->lock);

  mode_t mask = S_ISUID | S_ISGID | S_ISVTX |
                S_IRUSR | S_IWUSR | S_IXUSR |
                S_IRGRP | S_IWGRP | S_IXGRP |
                S_IROTH | S_IWOTH | S_IXOTH;

  node->vstat.st_mode = (node->vstat.st_mode & ~mask) | (mode & mask);

  update_times(node, U_CTIME);

  rwmutex_write_unlock(&node->lock);
  rwmutex_read_unlock(&the_fs.lock);
  return 0;
}

static int memfs_chown(const char *path, uid_t uid, gid_t gid) {
  rwmutex_read_lock(&the_fs.lock);
  struct node *node;
  if(!getnodebypath(path, &the_fs, &node)) {
    int err=errno;
    rwmutex_read_unlock(&the_fs.lock);
    return -err;
  }
  rwmutex_write_lock(&node->lock);

  node->vstat.st_uid = uid;
  node->vstat.st_gid = gid;

  update_times(node, U_CTIME);

  rwmutex_write_unlock(&node->lock);

  rwmutex_read_unlock(&the_fs.lock);
  return 0;
}

static int memfs_utimens(const char *path, const struct timespec ts[2]) {
  rwmutex_read_lock(&the_fs.lock);
  struct node *node;
  if(!getnodebypath(path, &the_fs, &node)) {
    int err= errno;
    rwmutex_read_unlock(&the_fs.lock);
    return -err;
  }
  rwmutex_write_lock(&node->lock);

  node->vstat.st_atime = ts[0].tv_sec;
  node->vstat.st_mtime = ts[1].tv_sec;
  rwmutex_write_unlock(&node->lock);
  rwmutex_read_unlock(&the_fs.lock);
  return 0;
}

static int memfs_truncate(const char *path, off_t size) {
  rwmutex_read_lock(&the_fs.lock);
  struct node *node;
  if(!getnodebypath(path, &the_fs, &node)) {
    int err=errno;
    rwmutex_read_unlock(&the_fs.lock);
    return -err;
  }
  rwmutex_write_lock(&node->lock);
  node->fd_count++;
  rwmutex_read_unlock(&the_fs.lock);

  // Calculate new block count
  blkcnt_t newblkcnt = (size + BLOCKSIZE - 1) / BLOCKSIZE;
  blkcnt_t oldblkcnt = node->vstat.st_blocks;

  if(oldblkcnt != newblkcnt) {
    if (newblkcnt == 0) {
      free(node->data);
      node->data = NULL;
    } else {
      void *newdata = calloc(newblkcnt, BLOCKSIZE); 
      if(!newdata) {
        goto out_err;
      }
      if (node->data != NULL && node->vstat.st_size > 0) {
        size_t copy_bytes = (node->vstat.st_size < size) ? node->vstat.st_size : size;
        memcpy(newdata, node->data, copy_bytes);
      }
      free(node->data);
      node->data = newdata;
    }
  } else if (node->data != NULL && node->vstat.st_size < size) {
    memset((char *) node->data + node->vstat.st_size, 0, size - node->vstat.st_size);
  }

  node->vstat.st_size = size;
  node->vstat.st_blocks = newblkcnt;
  update_times(node, U_CTIME | U_MTIME);

  int should_free = 0;
  if(--node->fd_count == 0 && node->delete_on_close) {
    should_free = 1;
  }
  rwmutex_write_unlock(&node->lock);

  if (should_free) {
    destroy_node(node);
  }
  return 0;

out_err:
  {
    int should_free_err = 0;
    if(--node->fd_count == 0 && node->delete_on_close) {
      should_free_err = 1;
    }
    rwmutex_write_unlock(&node->lock);
    if (should_free_err) destroy_node(node);
    return -ENOMEM;
  }
}


static int memfs_open_locked(const char *path, struct fuse_file_info *fi) {
  struct node *node;
  fprintf(stderr, "[open] path: %s\n", path);
  if(!getnodebypath(path, &the_fs, &node)) {
    return -errno;
  }
  rwmutex_write_lock(&node->lock);

  if(!S_ISREG(node->vstat.st_mode)) {
    if(S_ISDIR(node->vstat.st_mode)) {
      rwmutex_write_unlock(&node->lock);
      return -EISDIR;
    }
  }

  // Update file timestamps
  update_times(node, U_ATIME);

  // The "file handle" is a pointer to a struct we use to keep track of the inode and the
  // flags passed to open().
  struct filehandle *fh = malloc(sizeof(struct filehandle));
  fh->node    = node;
  fh->o_flags = fi->flags;

  fi->fh = (uint64_t) fh;

  node->fd_count++;
  rwmutex_write_unlock(&node->lock);

  return 0;
}
static int memfs_open(const char *path, struct fuse_file_info *fi){
  rwmutex_read_lock(&the_fs.lock);
  int ret= memfs_open_locked(path,fi);
  rwmutex_read_unlock(&the_fs.lock);
  return ret;
}

static int memfs_create(const char *path, mode_t mode,
                        struct fuse_file_info *fi) {
                          rwmutex_write_lock(&the_fs.lock);
    int ret;
    fprintf(stderr, "[create] path: %s\n", path);
    
    /* Create a regular file */
    ret = memfs_mknod_locked(path, S_IFREG | mode, 0);
    if (ret != 0){
        rwmutex_write_unlock(&the_fs.lock);
        return ret;
    }
    /* Open the newly created file */
    ret = memfs_open_locked(path, fi);
    if (ret != 0) {
        /* Roll back creation if opening fails */
        memfs_unlink_locked(path);
        rwmutex_write_unlock(&the_fs.lock);
        return ret;
    }
    rwmutex_write_unlock(&the_fs.lock);
    return 0;
}

static int memfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
  
  struct filehandle *fh = (struct filehandle *) fi->fh;

  // Check whether the file was opened for reading
  if(!O_READ(fh->o_flags)) {
    
    return -EACCES;
  }

  struct node *node = fh->node;
  rwmutex_read_lock(&node->lock);

  off_t filesize = node->vstat.st_size;

  // Check whether offset is at or beyond the end of file
  if(offset >= filesize) {
    rwmutex_read_unlock(&node->lock);
    return 0;
  }

  // Calculate number of bytes to copy
  size_t avail = filesize - offset;
  size_t n = (size < avail) ? size : avail;

  // Copy file contents
  memcpy(buf, node->data + offset, n);

  update_times(node, U_ATIME);

  rwmutex_read_unlock(&node->lock);
  return n;
}

static int memfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
 
  struct filehandle *fh = (struct filehandle *) fi->fh;

  // Check whether the file was opened for writing
  if(!O_WRITE(fh->o_flags)) {
    
    return -EACCES;
  }

  struct node *node = fh->node;
  rwmutex_write_lock(&node->lock);

  // Calculate number of required blocks
  blkcnt_t req_blocks = (offset + size + BLOCKSIZE - 1) / BLOCKSIZE;

  if(node->vstat.st_blocks < req_blocks) {
    // Allocate more memory
    void *newdata = malloc(req_blocks * BLOCKSIZE);
    if(!newdata) {
      rwmutex_write_unlock(&node->lock);
      return -ENOMEM;
    }

    // Copy old contents
    if(node->data != NULL) {
      memcpy(newdata, node->data, node->vstat.st_size);
      free(node->data);
    }

    // Update allocation information
    node->data = newdata;
    node->vstat.st_blocks = req_blocks;
  }

  // Write to file buffer
  memcpy(((char *) node->data) + offset, buf, size);

  // Update file size if necessary
  off_t minsize = offset + size;
  if(minsize > node->vstat.st_size) {
    node->vstat.st_size = minsize;
  }

  update_times(node, U_CTIME | U_MTIME);

  rwmutex_write_unlock(&node->lock);
  return size;
}

static int memfs_release(const char *path, struct fuse_file_info *fi) {
  
  struct filehandle *fh = (struct filehandle *) fi->fh;
  struct node *node = fh->node;

  // If the file was deleted but we could not free it due to open file descriptors,
  // free the node and its data after all file descriptors have been closed.
 
  int should_free = 0;

  rwmutex_write_lock(&node->lock); 
  if(--node->fd_count == 0) {
    if(node->delete_on_close) {
      should_free = 1;
    }
  }
  rwmutex_write_unlock(&node->lock); 

  if (should_free) {
    destroy_node(node); 
  }

  free(fh);
  return 0;
}

static struct fuse_operations memfs_oper = {
  .getattr      = memfs_getattr,
  .readlink     = memfs_readlink,
  .readdir      = memfs_readdir,
  .mknod        = memfs_mknod,
  .mkdir        = memfs_mkdir,
  .symlink      = memfs_symlink,
  .unlink       = memfs_unlink,
  .rmdir        = memfs_rmdir,
  .rename       = memfs_rename,
  .link         = memfs_link,
  .chmod        = memfs_chmod,
  .chown        = memfs_chown,
  .truncate     = memfs_truncate,
  .utimens      = memfs_utimens,
  .open         = memfs_open,
  .create       = memfs_create,
  .read         = memfs_read,
  .write        = memfs_write,
  .release      = memfs_release
};

//
// Application entry point
//F

static void *statistics_thread(void *arg) {
  (void)arg;

  while(1){
    sleep(5);

    rwmutex_read_lock(&the_fs.lock);
    collect_statistics(the_fs.root);
    rwmutex_read_unlock(&the_fs.lock);
  }
  return NULL;
}
int main(int argc,char *argv[]){
  // Initialize root directory
  struct node *root = malloc(sizeof(struct node));

  memset(root, 0, sizeof(struct node));
  initstat(root, S_IFDIR | 0755);
  root->vstat.st_uid = getuid();
  root->vstat.st_gid = getgid();

  // No entries
  root->data = NULL;
  rwmutex_init(&root->lock);

  // Set root directory of filesystem
  the_fs.root = root;

  rwmutex_init(&the_fs.lock);

  pthread_t stats_tid;
  pthread_create(&stats_tid,NULL,statistics_thread,NULL);
  pthread_detach(stats_tid);

  umask(0);
  return fuse_main(argc, argv, &memfs_oper, NULL);
}

