/* ai.c - the AI link (see ai.h) */

#include <stdio.h>
#include <string.h>
#include "ai.h"
#include "vfs.h"
#include "charset.h"
#include "fileserver.h"
#include "pw_protocol.h"

#define REPLY_MAX  (64 * 1024)          /* longest answer kept, Latin-1 */
#define ERR_MAX    200

static int      hello;                  /* the bridge announced itself */
static int      busy;                   /* a question is out */
static int      done;                   /* "END" arrived */

/* the answer so far (Latin-1), read by the PS1 from rd to wr */
static uint8_t  reply[REPLY_MAX];
static uint32_t reply_wr, reply_rd;
static int      truncated;

static char     err[ERR_MAX + 1];
static int      err_len, err_set;

/* input parser */
static char     line[80];
static int      line_len;
static enum { IN_LINE, IN_TXT, IN_ERR } in_mode;
static uint32_t in_left;
static utf8_dec_t dec;

void ai_init(void) {
	hello = busy = done = 0;
	reply_wr = reply_rd = 0;
	truncated = 0;
	err_set = err_len = 0;
	line_len = 0;
	in_mode = IN_LINE;
}

int ai_ready(void) { return hello; }

static void set_error(const char *msg) {
	snprintf(err, sizeof err, "%s", msg);
	err_len = (int)strlen(err);
	err_set = 1;
}

/* one byte of answer text (UTF-8) -> Latin-1 into reply[] or err[] */
static void text_byte(uint8_t b, int to_err) {
	uint32_t u;
	int r = utf8_decode(&dec, b, &u);
	if (r == 0 || u == '\r')
		return;
	char one[2] = { '?', 0 };
	const char *s = one;
	int c = (r > 0) ? unicode_to_latin1(u) : -1;
	if (c >= 0)
		one[0] = (char)c;
	else if (r > 0 && unicode_fallback(u))
		s = unicode_fallback(u);
	for (; *s; s++) {
		if (to_err) {
			if (err_len < ERR_MAX)
				err[err_len++] = *s, err[err_len] = 0;
		} else if (busy) {
			if (reply_wr < REPLY_MAX)
				reply[reply_wr++] = (uint8_t)*s;
			else
				truncated = 1;
		}
	}
}

static void header(void) {
	unsigned n = 0;
	line[line_len] = 0;
	if (strncmp(line, "HELLO", 5) == 0) {
		hello = 1;
	} else if (sscanf(line, "TXT %u", &n) == 1) {
		in_mode = IN_TXT;
		in_left = n;
		dec.need = 0;
	} else if (sscanf(line, "ERR %u", &n) == 1) {
		in_mode = IN_ERR;
		in_left = n;
		err_len = 0;
		err[0] = 0;
		dec.need = 0;
	} else if (strcmp(line, "END") == 0) {
		if (busy)
			done = 1;
	}
	if (in_mode != IN_LINE && in_left == 0) {
		if (in_mode == IN_ERR && busy)
			err_set = 1;
		in_mode = IN_LINE;
	}
}

void ai_poll(void) {
	uint8_t buf[64];
	int n;

	if (!aiport_connected()) {
		if (hello && busy)
			set_error("The PC bridge disconnected.");
		hello = 0;
		line_len = 0;
		in_mode = IN_LINE;
		return;
	}
	while ((n = aiport_read(buf, sizeof buf)) > 0) {
		for (int i = 0; i < n; i++) {
			const uint8_t b = buf[i];
			if (in_mode == IN_LINE) {
				if (b == '\n') {
					header();
					line_len = 0;
				} else if (b != '\r' && line_len < (int)sizeof line - 1) {
					line[line_len++] = (char)b;
				}
				continue;
			}
			text_byte(b, in_mode == IN_ERR);
			if (--in_left == 0) {
				if (in_mode == IN_ERR && busy)
					err_set = 1;
				in_mode = IN_LINE;
			}
		}
	}
}

/* send the question: header, the saved document, the question text */
static int ask(const uint8_t *a, int alen) {
	char path[160], hdr[96];
	uint8_t chunk[512];
	vfs_file *f = 0;
	uint32_t doclen = 0, got;

	const uint32_t para = pw_get32(a), off = pw_get32(a + 4);
	const uint8_t *q = a + 8;
	const int qn = alen - 8;

	/* the question in UTF-8 (Latin-1 needs at most 2 bytes per char) */
	static uint8_t q8[2 * PW_AI_MAX_QUESTION];
	int q8n = 0;
	for (int i = 0; i < qn && i < PW_AI_MAX_QUESTION; i++)
		q8n += utf8_encode(latin1_to_unicode(q[i]), q8 + q8n);

	if (fileserver_doc_path(path, sizeof path) && vfs_open(&f, path, VFS_READ) == 0)
		doclen = vfs_size(f);
	else
		f = 0;

	snprintf(hdr, sizeof hdr, "ASK %lu %lu %lu %d\n", (unsigned long)para,
	         (unsigned long)off, (unsigned long)doclen, q8n);
	int r = aiport_write(hdr, (uint32_t)strlen(hdr));
	uint32_t sent = 0;
	while (r == 0 && f && sent < doclen) {
		uint32_t want = doclen - sent < sizeof chunk ? doclen - sent : sizeof chunk;
		if (vfs_read(f, chunk, want, &got) || got == 0)
			break;
		r = aiport_write(chunk, got);
		sent += got;
	}
	while (r == 0 && sent < doclen) {         /* keep the promised length */
		memset(chunk, ' ', sizeof chunk);
		uint32_t k = doclen - sent < sizeof chunk ? doclen - sent : sizeof chunk;
		r = aiport_write(chunk, k);
		sent += k;
	}
	if (f)
		vfs_close(f);
	if (r == 0)
		r = aiport_write(q8, (uint32_t)q8n);
	return r;
}

uint16_t ai_process(uint8_t op, const uint8_t *a, int alen, uint8_t *out) {
	switch (op) {
	case PW_OP_AI_ASK:
		if (alen < 8)
			return (out[0] = PW_ERR_BAD_ARGS, 1);
		if (!hello)
			return (out[0] = PW_ERR_NOT_AVAILABLE, 1);
		busy = 1;
		done = 0;
		reply_wr = reply_rd = 0;
		truncated = 0;
		err_set = 0;
		if (ask(a, alen)) {
			busy = 0;
			return (out[0] = PW_ERR_IO, 1);
		}
		return (out[0] = PW_OK, 1);

	case PW_OP_AI_POLL: {
		out[0] = PW_OK;
		if (err_set) {
			out[1] = PW_AI_ERROR;
			memcpy(out + 2, err, (size_t)err_len);
			err_set = 0;
			busy = 0;
			return (uint16_t)(2 + err_len);
		}
		if (!busy) {
			out[1] = PW_AI_IDLE;
			return 2;
		}
		uint32_t n = reply_wr - reply_rd;
		if (n > PW_AI_CHUNK)
			n = PW_AI_CHUNK;
		memcpy(out + 2, reply + reply_rd, n);
		reply_rd += n;
		if (done && reply_rd == reply_wr) {
			out[1] = PW_AI_DONE;
			busy = 0;
			if (truncated && n + 30 < PW_AI_CHUNK) {
				static const char t[] = "\n[answer cut: too long]";
				memcpy(out + 2 + n, t, sizeof t - 1);
				n += sizeof t - 1;
			}
		} else {
			out[1] = PW_AI_MORE;
		}
		return (uint16_t)(2 + n);
	}

	case PW_OP_AI_CANCEL:
		if (busy && hello)
			aiport_write("CANCEL\n", 7);
		busy = 0;
		err_set = 0;
		return (out[0] = PW_OK, 1);
	}
	return (out[0] = PW_ERR_BAD_ARGS, 1);
}
