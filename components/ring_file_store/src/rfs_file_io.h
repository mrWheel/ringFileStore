#ifndef RFS_FILE_IO_H
#define RFS_FILE_IO_H

//-- Internal indirection over the storage backend syscalls.
//-- The component calls all file operations through this table so the host
//-- test suite can inject failures around every write, sync and rename
//-- boundary. NULL members fall through to the newlib implementations, so
//-- production behavior is the plain VFS path. This header is internal to
//-- the component; it is not part of the public API.

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  int (*open)(const char *path, int flags, int mode);
  int (*close)(int fd);
  ssize_t (*read)(int fd, void *buf, size_t count);
  ssize_t (*write)(int fd, const void *buf, size_t count);
  off_t (*lseek)(int fd, off_t offset, int whence);
  int (*fsync)(int fd);
  int (*rename)(const char *fromPath, const char *toPath);
  int (*unlink)(const char *path);
} rfsIoHooks_t;

//-- Replace the whole table (test only). Pass NULL to restore the plain
//-- VFS path.
void ringFileStoreSetIoHooks(const rfsIoHooks_t *hooks);

//-- Test inspection: cumulative counts through the hook table.
typedef struct
{
  uint32_t writes;
  uint32_t writeBytes;
  uint32_t fsyncs;
  uint32_t renames;
  uint32_t unlinks;
  uint32_t reads;
} rfsIoStats_t;

void ringFileStoreGetIoStats(rfsIoStats_t *outStats);
void ringFileStoreResetIoStats(void);

#ifdef __cplusplus
}
#endif

#endif
