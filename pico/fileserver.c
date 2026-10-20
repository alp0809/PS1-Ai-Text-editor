/*
 * fileserver.c - document storage for the PS1 editor.
 *
 * The PS1 works on a Latin-1 copy of the open document (the "working
 * copy"). Saving sends only what changed: a list of KEEP (copy a range of
 * the working copy) and BYTES (new text) pieces. The Pico builds the new
 * working copy from those, checks size and CRC-32 against what the PS1
 * computed, then puts the UTF-8 document (written alongside) in place and
 * rotates the backups.
 */

#include <string.h>
#include <stdio.h>
#include "fileserver.h"
#include "vfs.h"
#include "shared.h"
#include "charset.h"
#include "pw_protocol.h"
#include "ai.h"

#ifdef PICO_BUILD
#define LOG(...) printf(__VA_ARGS__)
#else
#define LOG(...) do { } while (0)
#endif

#define WORK_PATH  FS_WORK_DIR "/doc.latin1"
#define NEW_PATH   FS_WORK_DIR "/doc.new"
#define OUT_PATH   FS_WORK_DIR "/out.tmp"
#define LAST_PATH  FS_WORK_DIR "/last.txt"
#define CONF_PATH  FS_WORK_DIR "/settings.bin"

#define PATH_MAX_LEN 160

static char      cur_name[PW_MAX_NAME * 2 + 1];   /* UTF-8, empty = none */
static uint8_t   iobuf[512];

/* save in progress */
static int       saving;
static vfs_file *sv_old, *sv_new, *sv_out;   /* old copy, new copy, new UTF-8 */
static uint32_t  sv_old_size, sv_size, sv_crc;
static uint16_t  sv_next_chunk;

/* ======================================================================
 * names and conversions
 * ====================================================================== */

/* Latin-1 name from the PS1 -> UTF-8. Returns 0 if the name is not allowed. */
static int name_to_utf8(const uint8_t *in, int n, char *out, int outsz) {
	if (n < 1 || n > PW_MAX_NAME || in[0] == '.' || in[0] == ' ')
		return 0;
	int o = 0;
	for (int i = 0; i < n; i++) {
		uint8_t c = in[i];
		if (c < 0x20 || c == 0x7F || strchr("/\\:*?\"<>|", c))
			return 0;
		if (o + 4 >= outsz)
			return 0;
		o += utf8_encode(latin1_to_unicode(c), (uint8_t *)out + o);
	}
	out[o] = 0;
	return 1;
}

/* UTF-8 name -> Latin-1 for the PS1 (ASCII stand-ins or '?' where
 * impossible; such names are found again by open_lookup()). Returns length. */
static int name_to_latin1(const char *in, uint8_t *out, int outsz) {
	utf8_dec_t d = { 0, 0 };
	int o = 0;
	for (const uint8_t *p = (const uint8_t *)in; *p && o < outsz; p++) {
		uint32_t u;
		int r = utf8_decode(&d, *p, &u);
		if (r == 0)
			continue;
		int c = (r > 0) ? unicode_to_latin1(u) : -1;
		const char *f = (c < 0 && r > 0) ? unicode_fallback(u) : 0;
		if (c >= 0)
			out[o++] = (uint8_t)c;
		else if (f && *f)
			for (; *f && o < outsz; f++)
				out[o++] = (uint8_t)*f;
		else
			out[o++] = '?';
	}
	return o;
}

static void doc_path(char *out, const char *name) {
	snprintf(out, PATH_MAX_LEN, "%s/%s", FS_DOC_DIR, name);
}

static void bak_path(char *out, const char *name, int n) {
	snprintf(out, PATH_MAX_LEN, "%s/%s.%d", FS_BAK_DIR, name, n);
}

/* UTF-8 document -> Latin-1 working copy. Drops '\r', maps typographic
 * characters to ASCII, '?' for anything else Latin-1 can't show.
 * Returns PW_OK and the working copy size. */
static int convert_in(const char *src, const char *dst, uint32_t *size_out) {
	vfs_file *in, *out;
	if (vfs_open(&in, src, VFS_READ))
		return PW_ERR_NOT_FOUND;
	if (vfs_open(&out, dst, VFS_WRITE)) {
		vfs_close(in);
		return PW_ERR_IO;
	}

	utf8_dec_t d = { 0, 0 };
	uint8_t outbuf[256];
	int on = 0, err = PW_OK;
	uint32_t total = 0, got;

	while (err == PW_OK && vfs_read(in, iobuf, sizeof iobuf, &got) == 0 && got > 0) {
		for (uint32_t i = 0; i < got; i++) {
			uint32_t u;
			int r = utf8_decode(&d, iobuf[i], &u);
			if (r == 0)
				continue;
			if (u == '\r')
				continue;

			char one[2] = { '?', 0 };
			const char *s = one;
			int c = (r > 0) ? unicode_to_latin1(u) : -1;
			if (c >= 0)
				one[0] = (char)c;
			else if (r > 0 && unicode_fallback(u))
				s = unicode_fallback(u);

			for (; *s; s++) {
				outbuf[on++] = (uint8_t)*s;
				if (on == (int)sizeof outbuf) {
					if (vfs_write(out, outbuf, on)) { err = PW_ERR_IO; break; }
					total += on;
					on = 0;
				}
			}
			if (err)
				break;
		}
		if (total > FS_MAX_DOC)
			err = PW_ERR_TOO_BIG;
	}
	if (err == PW_OK && on) {
		if (vfs_write(out, outbuf, on))
			err = PW_ERR_IO;
		total += on;
	}
	vfs_close(in);
	vfs_close(out);
	if (err == PW_OK && total > FS_MAX_DOC)
		err = PW_ERR_TOO_BIG;
	*size_out = total;
	return err;
}

static void remember_last(void) {
	vfs_file *f;
	if (vfs_open(&f, LAST_PATH, VFS_WRITE) == 0) {
		vfs_write(f, cur_name, (uint32_t)strlen(cur_name));
		vfs_close(f);
	}
}

static int read_last(char *out, int outsz) {
	vfs_file *f;
	uint32_t got = 0;
	if (vfs_open(&f, LAST_PATH, VFS_READ))
		return 0;
	vfs_read(f, out, (uint32_t)outsz - 1, &got);
	vfs_close(f);
	out[got] = 0;
	return got > 0;
}

static void abort_save(void) {
	if (sv_old) vfs_close(sv_old);
	if (sv_new) vfs_close(sv_new);
	if (sv_out) vfs_close(sv_out);
	sv_old = sv_new = sv_out = 0;
	if (saving) {
		vfs_remove(NEW_PATH);
		vfs_remove(OUT_PATH);
	}
	saving = 0;
}

/* ======================================================================
 * operations
 * ====================================================================== */

/* sorted file list, rebuilt on every LIST with start == 0 */
#define LIST_MAX 128
static struct { char name[PW_MAX_NAME * 2 + 1]; uint32_t size; } list[LIST_MAX];
static int list_n;

static int list_cb(const char *name, uint32_t size, int is_dir, void *ctx) {
	(void)ctx;
	if (is_dir || name[0] == '.' || list_n >= LIST_MAX)
		return 0;
	if (strlen(name) >= sizeof list[0].name)
		return 0;
	/* insertion sort by name */
	int i = list_n++;
	while (i > 0 && strcmp(list[i - 1].name, name) > 0) {
		list[i] = list[i - 1];
		i--;
	}
	strcpy(list[i].name, name);
	list[i].size = size;
	return 0;
}

static uint16_t op_list(const uint8_t *a, int alen, uint8_t *out) {
	if (alen < 2)
		return (out[0] = PW_ERR_BAD_ARGS, 1);
	int start = pw_get16(a);
	if (start == 0) {
		list_n = 0;
		vfs_list(FS_DOC_DIR, list_cb, 0);
	}
	out[0] = PW_OK;
	pw_put16(out + 1, (uint16_t)list_n);
	int o = 4, count = 0;
	for (int i = start; i < list_n; i++) {
		uint8_t nm[PW_MAX_NAME];
		int nl = name_to_latin1(list[i].name, nm, PW_MAX_NAME);
		if (o + 1 + nl + 4 > PW_MAX_PAYLOAD - 3)
			break;
		out[o++] = (uint8_t)nl;
		memcpy(out + o, nm, nl);
		o += nl;
		pw_put32(out + o, list[i].size);
		o += 4;
		count++;
	}
	out[3] = (uint8_t)count;
	return (uint16_t)o;
}

static int open_lookup(const uint8_t *a, int alen, char *name, int namesz) {
	list_n = 0;
	vfs_list(FS_DOC_DIR, list_cb, 0);
	for (int i = 0; i < list_n; i++) {
		uint8_t nm[PW_MAX_NAME];
		int nl = name_to_latin1(list[i].name, nm, PW_MAX_NAME);
		if (nl == alen && memcmp(nm, a, (size_t)alen) == 0 && (int)strlen(list[i].name) < namesz) {
			strcpy(name, list[i].name);
			return 1;
		}
	}
	return 0;
}

static uint16_t op_open(const uint8_t *a, int alen, uint8_t *out) {
	char name[sizeof cur_name], path[PATH_MAX_LEN];
	abort_save();
	if (alen == 0) {
		if (!read_last(name, sizeof name))
			return (out[0] = PW_ERR_NOT_FOUND, 1);
	} else {
		int ok = name_to_utf8(a, alen, name, sizeof name);
		if (ok) {
			doc_path(path, name);
			ok = vfs_exists(path);
		}
		/* a name with characters Latin-1 lacks reaches us as the PS1 saw it
		 * in the list: find the file whose Latin-1 name matches */
		if (!ok && !open_lookup(a, alen, name, sizeof name))
			return (out[0] = PW_ERR_NOT_FOUND, 1);
	}
	doc_path(path, name);
	uint32_t size;
	int err = convert_in(path, WORK_PATH, &size);
	if (err) {
		if (err != PW_ERR_NOT_FOUND)
			cur_name[0] = 0;          /* the working copy is gone */
		return (out[0] = (uint8_t)err, 1);
	}
	strcpy(cur_name, name);
	remember_last();
	LOG("open %s (%lu bytes)\n", cur_name, (unsigned long)size);

	out[0] = PW_OK;
	pw_put32(out + 1, size);
	int nl = name_to_latin1(cur_name, out + 6, PW_MAX_NAME);
	out[5] = (uint8_t)nl;
	return (uint16_t)(6 + nl);
}

static uint16_t op_read(const uint8_t *a, int alen, uint8_t *out) {
	if (alen < 6 || !cur_name[0])
		return (out[0] = (uint8_t)(cur_name[0] ? PW_ERR_BAD_ARGS : PW_ERR_BAD_STATE), 1);
	uint32_t off = pw_get32(a);
	uint32_t n = pw_get16(a + 4);
	if (n > PW_READ_CHUNK)
		n = PW_READ_CHUNK;
	vfs_file *f;
	if (vfs_open(&f, WORK_PATH, VFS_READ))
		return (out[0] = PW_ERR_IO, 1);
	uint32_t got = 0;
	int err = vfs_seek(f, off) || vfs_read(f, out + 1, n, &got);
	vfs_close(f);
	if (err)
		return (out[0] = PW_ERR_IO, 1);
	out[0] = PW_OK;
	return (uint16_t)(1 + got);
}

static uint16_t op_create(const uint8_t *a, int alen, uint8_t *out) {
	char name[sizeof cur_name], path[PATH_MAX_LEN];
	abort_save();
	if (!name_to_utf8(a, alen, name, sizeof name))
		return (out[0] = PW_ERR_BAD_ARGS, 1);
	doc_path(path, name);
	if (vfs_exists(path))
		return (out[0] = PW_ERR_EXISTS, 1);
	vfs_file *f;
	if (vfs_open(&f, path, VFS_WRITE))
		return (out[0] = PW_ERR_IO, 1);
	vfs_close(f);
	if (vfs_open(&f, WORK_PATH, VFS_WRITE))
		return (out[0] = PW_ERR_IO, 1);
	vfs_close(f);
	strcpy(cur_name, name);
	remember_last();
	LOG("created %s\n", cur_name);
	return (out[0] = PW_OK, 1);
}

static uint16_t op_save_begin(uint8_t *out) {
	abort_save();
	if (!cur_name[0])
		return (out[0] = PW_ERR_BAD_STATE, 1);
	if (vfs_open(&sv_old, WORK_PATH, VFS_READ)) {
		sv_old = 0;
		return (out[0] = PW_ERR_IO, 1);
	}
	if (vfs_open(&sv_new, NEW_PATH, VFS_WRITE)) {
		vfs_close(sv_old);
		sv_old = sv_new = 0;
		return (out[0] = PW_ERR_IO, 1);
	}
	if (vfs_open(&sv_out, OUT_PATH, VFS_WRITE)) {
		vfs_close(sv_old);
		vfs_close(sv_new);
		sv_old = sv_new = sv_out = 0;
		vfs_remove(NEW_PATH);
		return (out[0] = PW_ERR_IO, 1);
	}
	sv_old_size = vfs_size(sv_old);
	sv_size = 0;
	sv_crc = 0;
	sv_next_chunk = 0;
	saving = 1;
	return (out[0] = PW_OK, 1);
}

/* new text: to the Latin-1 working copy and, converted, to the UTF-8 file
 * (in one pass, so saving reads and writes the card only once) */
static int sv_write(const uint8_t *p, uint32_t n) {
	static uint8_t u8[2 * sizeof iobuf];   /* Latin-1 needs at most 2 UTF-8 bytes */
	sv_size += n;
	sv_crc = pw_crc32(sv_crc, p, n);
	if (sv_size > FS_MAX_DOC)
		return PW_ERR_TOO_BIG;
	if (vfs_write(sv_new, p, n))
		return PW_ERR_IO;
	while (n > 0) {
		uint32_t k = n > sizeof iobuf ? sizeof iobuf : n;
		int on = 0;
		for (uint32_t i = 0; i < k; i++)
			on += utf8_encode(latin1_to_unicode(p[i]), u8 + on);
		if (vfs_write(sv_out, u8, (uint32_t)on))
			return PW_ERR_IO;
		p += k;
		n -= k;
	}
	return PW_OK;
}

static uint16_t op_save_data(const uint8_t *a, int alen, uint8_t *out) {
	if (!saving)
		return (out[0] = PW_ERR_BAD_STATE, 1);
	if (alen < 2)
		return (out[0] = PW_ERR_BAD_ARGS, 1);
	uint16_t chunk = pw_get16(a);
	if (chunk + 1 == sv_next_chunk)          /* a resend: already applied */
		return (out[0] = PW_OK, 1);
	if (chunk != sv_next_chunk) {
		abort_save();
		return (out[0] = PW_ERR_BAD_ARGS, 1);
	}

	int i = 2, err = PW_OK;
	while (i < alen && err == PW_OK) {
		uint8_t type = a[i++];
		if (type == PW_SAVE_KEEP && i + 8 <= alen) {
			uint32_t off = pw_get32(a + i), len = pw_get32(a + i + 4);
			i += 8;
			if (off > sv_old_size || len > sv_old_size - off || vfs_seek(sv_old, off)) {
				err = PW_ERR_BAD_ARGS;
				break;
			}
			while (len && err == PW_OK) {
				uint32_t n = len > sizeof iobuf ? sizeof iobuf : len, got = 0;
				if (vfs_read(sv_old, iobuf, n, &got) || got != n) {
					err = PW_ERR_IO;
					break;
				}
				err = sv_write(iobuf, n);
				len -= n;
			}
		} else if (type == PW_SAVE_BYTES && i + 2 <= alen) {
			uint16_t len = pw_get16(a + i);
			i += 2;
			if (i + len > alen) {
				err = PW_ERR_BAD_ARGS;
				break;
			}
			err = sv_write(a + i, len);
			i += len;
		} else {
			err = PW_ERR_BAD_ARGS;
		}
	}
	if (err) {
		abort_save();
		return (out[0] = (uint8_t)err, 1);
	}
	sv_next_chunk++;
	return (out[0] = PW_OK, 1);
}

static uint16_t op_save_end(const uint8_t *a, int alen, uint8_t *out) {
	if (!saving)
		return (out[0] = PW_ERR_BAD_STATE, 1);
	if (alen < 8) {
		abort_save();
		return (out[0] = PW_ERR_BAD_ARGS, 1);
	}
	uint32_t want_size = pw_get32(a), want_crc = pw_get32(a + 4);

	vfs_close(sv_old);
	sv_old = 0;
	int err = vfs_close(sv_new) ? PW_ERR_IO : PW_OK;
	if (vfs_close(sv_out))
		err = PW_ERR_IO;
	sv_new = sv_out = 0;

	if (!err && (want_size != sv_size || want_crc != sv_crc)) {
		LOG("save verify failed: size %lu/%lu\n",
		    (unsigned long)sv_size, (unsigned long)want_size);
		err = PW_ERR_VERIFY;
	}
	if (err) {
		vfs_remove(NEW_PATH);
		vfs_remove(OUT_PATH);
		saving = 0;
		return (out[0] = (uint8_t)err, 1);
	}

	/* rotate backups: name.5 dropped, 4 -> 5 ... 1 -> 2, current -> 1 */
	char p1[PATH_MAX_LEN], p2[PATH_MAX_LEN], doc[PATH_MAX_LEN];
	bak_path(p1, cur_name, FS_BACKUPS);
	vfs_remove(p1);
	for (int n = FS_BACKUPS - 1; n >= 1; n--) {
		bak_path(p1, cur_name, n);
		bak_path(p2, cur_name, n + 1);
		if (vfs_exists(p1))
			vfs_rename(p1, p2);
	}
	doc_path(doc, cur_name);
	bak_path(p1, cur_name, 1);
	if (vfs_exists(doc))
		vfs_rename(doc, p1);

	/* new document into place, new working copy into place */
	if (vfs_rename(OUT_PATH, doc))
		err = PW_ERR_IO;
	vfs_remove(WORK_PATH);
	if (vfs_rename(NEW_PATH, WORK_PATH))
		err = PW_ERR_IO;
	saving = 0;
	if (err)
		return (out[0] = (uint8_t)err, 1);

	LOG("saved %s (%lu bytes)\n", cur_name, (unsigned long)sv_size);
	out[0] = PW_OK;
	pw_put32(out + 1, sv_size);
	return 5;
}

static uint16_t op_config_get(uint8_t *out) {
	vfs_file *f;
	uint32_t got = 0;
	out[0] = PW_OK;
	if (vfs_open(&f, CONF_PATH, VFS_READ) == 0) {
		vfs_read(f, out + 1, PW_CONFIG_MAX, &got);
		vfs_close(f);
	}
	return (uint16_t)(1 + got);
}

static uint16_t op_config_set(const uint8_t *a, int alen, uint8_t *out) {
	vfs_file *f;
	if (alen > PW_CONFIG_MAX)
		return (out[0] = PW_ERR_BAD_ARGS, 1);
	if (vfs_open(&f, CONF_PATH, VFS_WRITE))
		return (out[0] = PW_ERR_IO, 1);
	int err = vfs_write(f, a, (uint32_t)alen);
	if (vfs_close(f))
		err = 1;
	return (out[0] = (uint8_t)(err ? PW_ERR_IO : PW_OK), 1);
}

int fileserver_doc_path(char *out, int outsz) {
	if (!cur_name[0])
		return 0;
	snprintf(out, (size_t)outsz, "%s/%s", FS_DOC_DIR, cur_name);
	return 1;
}

/* ======================================================================
 * entry points
 * ====================================================================== */

void fileserver_init(void) {
	vfs_mkdir(FS_DOC_DIR);
	vfs_mkdir(FS_BAK_DIR);
	vfs_mkdir(FS_WORK_DIR);
	cur_name[0] = 0;
	saving = 0;
	sv_old = sv_new = sv_out = 0;
}

uint16_t fileserver_process(const uint8_t *req, uint16_t len, uint8_t *resp) {
	if (len < 2) {
		resp[0] = 0;
		resp[1] = 0;
		resp[2] = PW_ERR_BAD_ARGS;
		return 3;
	}
	const uint8_t op = req[0], *a = req + 2;
	const int alen = len - 2;
	uint8_t *out = resp + 2;            /* out[0] = result, then data */
	uint16_t n;

	resp[0] = op;
	resp[1] = req[1];                   /* echo the sequence number */

	if (op != PW_OP_HELLO && op < PW_OP_AI_ASK && !vfs_ready()) {
		out[0] = PW_ERR_NO_SD;
		return 3;
	}

	switch (op) {
	case PW_OP_HELLO:
		out[0] = PW_OK;
		out[1] = PW_PROTOCOL_VERSION;
		out[2] = pw_flags;
		pw_put16(out + 3, PW_MAX_PAYLOAD);
		n = 5;
		break;
	case PW_OP_LIST:       n = op_list(a, alen, out);      break;
	case PW_OP_OPEN:       n = op_open(a, alen, out);      break;
	case PW_OP_READ:       n = op_read(a, alen, out);      break;
	case PW_OP_CREATE:     n = op_create(a, alen, out);    break;
	case PW_OP_SAVE_BEGIN: n = op_save_begin(out);         break;
	case PW_OP_SAVE_DATA:  n = op_save_data(a, alen, out); break;
	case PW_OP_SAVE_END:   n = op_save_end(a, alen, out);  break;
	case PW_OP_CONFIG_GET: n = op_config_get(out);         break;
	case PW_OP_CONFIG_SET: n = op_config_set(a, alen, out); break;
	case PW_OP_AI_ASK:
	case PW_OP_AI_POLL:
	case PW_OP_AI_CANCEL:  n = ai_process(op, a, alen, out); break;
	default:
		out[0] = PW_ERR_BAD_ARGS;
		n = 1;
	}
	return (uint16_t)(2 + n);
}

int fileserver_poll(void) {
	if (mb_state != MB_PENDING)
		return 0;
	mb_state = MB_PROCESSING;
	MEM_BARRIER();
	uint16_t n = fileserver_process(mb_req, mb_req_len, mb_resp);
	mb_resp_len = n;
	MEM_BARRIER();
	mb_resp_valid = 1;
	MEM_BARRIER();
	mb_state = MB_IDLE;
	return 1;
}
