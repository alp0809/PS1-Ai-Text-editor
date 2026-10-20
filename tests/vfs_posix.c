/* vfs_posix.c - vfs.h on a folder of the PC (tests) */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "vfs.h"

struct vfs_file { FILE *f; };

static char root[512];
static int  ready;

void vfs_posix_root(const char *dir) {
	snprintf(root, sizeof root, "%s", dir);
	ready = 1;
}

void vfs_posix_set_ready(int r) { ready = r; }

static const char *full(const char *path) {
	static char buf[2][1024];
	static int k;
	k ^= 1;
	snprintf(buf[k], sizeof buf[k], "%s%s", root, path);
	return buf[k];
}

int vfs_ready(void) { return ready; }

int vfs_open(vfs_file **out, const char *path, int mode) {
	FILE *f = fopen(full(path), mode == VFS_WRITE ? "wb" : "rb");
	if (!f)
		return -1;
	*out = malloc(sizeof **out);
	(*out)->f = f;
	return 0;
}

int vfs_read(vfs_file *f, void *buf, uint32_t n, uint32_t *got) {
	*got = (uint32_t)fread(buf, 1, n, f->f);
	return ferror(f->f) ? -1 : 0;
}

int vfs_write(vfs_file *f, const void *buf, uint32_t n) {
	return fwrite(buf, 1, n, f->f) == n ? 0 : -1;
}

int vfs_seek(vfs_file *f, uint32_t off) { return fseek(f->f, (long)off, SEEK_SET) ? -1 : 0; }

uint32_t vfs_size(vfs_file *f) {
	long pos = ftell(f->f);
	fseek(f->f, 0, SEEK_END);
	long n = ftell(f->f);
	fseek(f->f, pos, SEEK_SET);
	return (uint32_t)n;
}

int vfs_close(vfs_file *f) {
	int r = fclose(f->f);
	free(f);
	return r ? -1 : 0;
}

int vfs_exists(const char *path) {
	struct stat st;
	return stat(full(path), &st) == 0;
}

int vfs_remove(const char *path) {
	unlink(full(path));
	return 0;
}

int vfs_rename(const char *from, const char *to) {
	if (vfs_exists(to))
		return -1;               /* FatFs refuses too */
	return rename(full(from), full(to)) ? -1 : 0;
}

int vfs_mkdir(const char *path) {
	mkdir(full(path), 0777);
	return 0;
}

int vfs_list(const char *dir, vfs_list_cb cb, void *ctx) {
	DIR *d = opendir(full(dir));
	struct dirent *e;
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char p[1024];
		struct stat st;
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(p, sizeof p, "%s/%s", full(dir), e->d_name);
		if (stat(p, &st))
			continue;
		if (cb(e->d_name, (uint32_t)st.st_size, S_ISDIR(st.st_mode), ctx))
			break;
	}
	closedir(d);
	return 0;
}
