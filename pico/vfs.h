/*
 * vfs.h - the few file operations the file server needs.
 * vfs_fatfs.c implements them on the SD card (Pico), vfs_posix.c on a PC
 * folder (tests). Paths are UTF-8, "/" separated, relative to the card root.
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>

typedef struct vfs_file vfs_file;

#define VFS_READ   0
#define VFS_WRITE  1      /* create or truncate */

int      vfs_ready(void);                         /* card mounted? */
int      vfs_open(vfs_file **f, const char *path, int mode);   /* 0 = ok */
int      vfs_read(vfs_file *f, void *buf, uint32_t n, uint32_t *got);
int      vfs_write(vfs_file *f, const void *buf, uint32_t n);
int      vfs_seek(vfs_file *f, uint32_t off);
uint32_t vfs_size(vfs_file *f);
int      vfs_close(vfs_file *f);

int      vfs_exists(const char *path);
int      vfs_remove(const char *path);            /* ok if missing */
int      vfs_rename(const char *from, const char *to);
int      vfs_mkdir(const char *path);             /* ok if it exists */

/* calls cb for every entry in dir; return non-zero from cb to stop */
typedef int (*vfs_list_cb)(const char *name, uint32_t size, int is_dir, void *ctx);
int      vfs_list(const char *dir, vfs_list_cb cb, void *ctx);

#endif
