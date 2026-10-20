/* vfs_fatfs.c - vfs.h on the SD card via FatFs (Pico build) */

#include <string.h>
#include "vfs.h"
#include "ff.h"

struct vfs_file {
	FIL fil;
	int used;
};

#define MAX_OPEN 4
static struct vfs_file pool[MAX_OPEN];
static FATFS fs;
static int mounted;

int vfs_mount(void) {
	if (mounted)
		return 1;
	if (f_mount(&fs, "", 1) == FR_OK)
		mounted = 1;
	else
		f_unmount("");
	return mounted;
}

int vfs_ready(void) { return mounted; }

int vfs_open(vfs_file **out, const char *path, int mode) {
	for (int i = 0; i < MAX_OPEN; i++) {
		if (!pool[i].used) {
			BYTE m = (mode == VFS_WRITE) ? (FA_WRITE | FA_CREATE_ALWAYS) : FA_READ;
			if (f_open(&pool[i].fil, path, m) != FR_OK)
				return -1;
			pool[i].used = 1;
			*out = &pool[i];
			return 0;
		}
	}
	return -1;
}

int vfs_read(vfs_file *f, void *buf, uint32_t n, uint32_t *got) {
	UINT g = 0;
	FRESULT r = f_read(&f->fil, buf, n, &g);
	*got = g;
	return r == FR_OK ? 0 : -1;
}

int vfs_write(vfs_file *f, const void *buf, uint32_t n) {
	UINT w = 0;
	return (f_write(&f->fil, buf, n, &w) == FR_OK && w == n) ? 0 : -1;
}

int vfs_seek(vfs_file *f, uint32_t off) {
	return f_lseek(&f->fil, off) == FR_OK ? 0 : -1;
}

uint32_t vfs_size(vfs_file *f) { return (uint32_t)f_size(&f->fil); }

int vfs_close(vfs_file *f) {
	FRESULT r = f_close(&f->fil);
	f->used = 0;
	return r == FR_OK ? 0 : -1;
}

int vfs_exists(const char *path) {
	FILINFO fi;
	return f_stat(path, &fi) == FR_OK;
}

int vfs_remove(const char *path) {
	FRESULT r = f_unlink(path);
	return (r == FR_OK || r == FR_NO_FILE || r == FR_NO_PATH) ? 0 : -1;
}

int vfs_rename(const char *from, const char *to) {
	return f_rename(from, to) == FR_OK ? 0 : -1;
}

int vfs_mkdir(const char *path) {
	FRESULT r = f_mkdir(path);
	return (r == FR_OK || r == FR_EXIST) ? 0 : -1;
}

int vfs_list(const char *dir, vfs_list_cb cb, void *ctx) {
	DIR d;
	FILINFO fi;
	if (f_opendir(&d, dir) != FR_OK)
		return -1;
	while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
		if (fi.fattrib & (AM_HID | AM_SYS))
			continue;
		if (cb(fi.fname, (uint32_t)fi.fsize, (fi.fattrib & AM_DIR) != 0, ctx))
			break;
	}
	f_closedir(&d);
	return 0;
}
