/* doc.c - paragraphs, loading and patch saving (see doc.h) */

#include <stdlib.h>
#include <string.h>
#include "doc.h"
#include "link.h"

#define T_LIST   3000
#define T_OPEN   120000      /* the Pico converts the whole file */
#define T_READ   3000
#define T_CREATE 5000
#define T_SAVE   120000      /* a big KEEP means a lot of copying on the card */
#define T_SAVE_END 120000

uint32_t doc_save_sent, doc_save_full;

/* ======================================================================
 * paragraphs
 * ====================================================================== */

static int is_space(uint8_t c) { return c == ' ' || c == '\t'; }

int doc_count_words(const uint8_t *t, int len) {
	int w = 0, in = 0;
	for (int i = 0; i < len; i++) {
		if (is_space(t[i]))
			in = 0;
		else if (!in)
			in = 1, w++;
	}
	return w;
}

static void para_free(para_t *p) {
	free(p->t);
	p->t = 0;
	p->len = p->cap = 0;
}

static int para_reserve(para_t *p, int need) {
	if (need <= p->cap)
		return 0;
	int cap = need + 32 + need / 8;
	uint8_t *t = realloc(p->t, (size_t)cap);
	if (!t)
		return -1;
	p->t = t;
	p->cap = cap;
	return 0;
}

static void touched(doc_t *d) {
	d->modified = 1;
	d->edit_gen++;
}

static void para_changed(doc_t *d, para_t *p) {
	d->words -= p->words;
	p->words = (uint16_t)doc_count_words(p->t, p->len);
	d->words += p->words;
	p->clean = 0;
	p->sgen = 0;
	touched(d);
}

/* make room for one paragraph at index pi (zeroed) */
static int para_open_slot(doc_t *d, int pi) {
	if (d->n == d->cap) {
		int cap = d->cap ? d->cap * 2 : 256;
		para_t *np = realloc(d->p, sizeof(para_t) * (size_t)cap);
		if (!np)
			return -1;
		d->p = np;
		d->cap = cap;
	}
	memmove(&d->p[pi + 1], &d->p[pi], sizeof(para_t) * (size_t)(d->n - pi));
	memset(&d->p[pi], 0, sizeof(para_t));
	d->n++;
	return 0;
}

static void para_close_slot(doc_t *d, int pi) {
	memmove(&d->p[pi], &d->p[pi + 1], sizeof(para_t) * (size_t)(d->n - pi - 1));
	d->n--;
}

void doc_init(doc_t *d) {
	memset(d, 0, sizeof *d);
	doc_clear(d);
}

void doc_clear(doc_t *d) {
	for (int i = 0; i < d->n; i++)
		para_free(&d->p[i]);
	d->n = 0;
	d->bytes = 0;
	d->words = 0;
	d->modified = 0;
	para_open_slot(d, 0);      /* cap >= 256 after the first call: can't fail */
	d->p[0].clean = 1;         /* an empty working copy */
}

void doc_free(doc_t *d) {
	for (int i = 0; i < d->n; i++)
		para_free(&d->p[i]);
	free(d->p);
	memset(d, 0, sizeof *d);
}

int doc_insert(doc_t *d, int pi, int off, const uint8_t *s, int n) {
	para_t *p = &d->p[pi];
	if (n <= 0)
		return 0;
	if (d->bytes + (uint32_t)n > DOC_MAX_BYTES || para_reserve(p, p->len + n))
		return -1;
	memmove(p->t + off + n, p->t + off, (size_t)(p->len - off));
	memcpy(p->t + off, s, (size_t)n);
	p->len += n;
	d->bytes += (uint32_t)n;
	para_changed(d, p);
	return 0;
}

void doc_erase(doc_t *d, int pi, int off, int n) {
	para_t *p = &d->p[pi];
	if (off + n > p->len)
		n = p->len - off;
	if (n <= 0)
		return;
	memmove(p->t + off, p->t + off + n, (size_t)(p->len - off - n));
	p->len -= n;
	d->bytes -= (uint32_t)n;
	para_changed(d, p);
}

int doc_split(doc_t *d, int pi, int off) {
	if (d->bytes + 1 > DOC_MAX_BYTES)
		return -1;
	if (off == 0) {
		/* new empty paragraph before; the old one keeps its origin */
		if (para_open_slot(d, pi))
			return -1;
	} else {
		int tail = d->p[pi].len - off;
		if (para_open_slot(d, pi + 1))
			return -1;
		para_t *a = &d->p[pi], *b = &d->p[pi + 1];
		if (tail > 0) {
			if (para_reserve(b, tail)) {
				para_close_slot(d, pi + 1);
				return -1;
			}
			memcpy(b->t, a->t + off, (size_t)tail);
			b->len = tail;
			a->len = off;
			para_changed(d, a);
		}
		para_changed(d, b);
	}
	d->bytes++;
	touched(d);
	return 0;
}

int doc_join(doc_t *d, int pi) {
	if (pi + 1 >= d->n)
		return 0;
	para_t *a = &d->p[pi], *b = &d->p[pi + 1];
	if (b->len > 0) {
		if (para_reserve(a, a->len + b->len))
			return -1;
		memcpy(a->t + a->len, b->t, (size_t)b->len);
		a->len += b->len;
		para_changed(d, a);
	}
	d->words -= b->words;
	para_free(b);
	para_close_slot(d, pi + 1);
	d->bytes--;
	touched(d);
	return 0;
}

/* ======================================================================
 * open / create
 * ====================================================================== */

static uint8_t buf[PW_MAX_PAYLOAD];

/* loader: a growing line buffer, one malloc per finished paragraph */
static uint8_t *lb;
static int lb_len, lb_cap;

static int finish_para(doc_t *d, uint32_t ooff, int onl) {
	if (para_open_slot(d, d->n))
		return -1;
	para_t *p = &d->p[d->n - 1];
	p->cap = lb_len > 8 ? lb_len : 8;
	p->t = malloc((size_t)p->cap);
	if (!p->t)
		return -1;
	if (lb_len)
		memcpy(p->t, lb, (size_t)lb_len);
	p->len = lb_len;
	p->ooff = ooff;
	p->olen = lb_len;
	p->clean = 1;
	p->onl = (uint8_t)onl;
	p->words = (uint16_t)doc_count_words(p->t, p->len);
	d->words += p->words;
	lb_len = 0;
	return 0;
}

int doc_open(doc_t *d, const uint8_t *name, int nlen, doc_progress_fn prog) {
	int r, n;
	r = link_call(PW_OP_OPEN, name, nlen, buf, sizeof buf, &n, T_OPEN);
	if (r != PW_OK)
		return r;
	if (n < 5)
		return LINK_ERR_PROTO;
	const uint32_t size = pw_get32(buf);
	int namelen = buf[4];
	if (namelen > PW_MAX_NAME || 5 + namelen > n)
		return LINK_ERR_PROTO;
	if (size > DOC_MAX_BYTES)
		return PW_ERR_TOO_BIG;

	/* from here on the old document is gone */
	for (int i = 0; i < d->n; i++)
		para_free(&d->p[i]);
	d->n = 0;
	d->words = 0;
	d->bytes = size;
	d->modified = 0;
	memcpy(d->name, buf + 5, (size_t)namelen);
	d->name[namelen] = 0;

	uint32_t off = 0, line_start = 0;
	lb_len = 0;
	r = PW_OK;
	while (off < size) {
		uint8_t args[6];
		uint32_t want = size - off;
		if (want > PW_READ_CHUNK)
			want = PW_READ_CHUNK;
		pw_put32(args, off);
		pw_put16(args + 4, (uint16_t)want);
		r = link_call(PW_OP_READ, args, 6, buf, sizeof buf, &n, T_READ);
		if (r == PW_OK && n == 0)
			r = LINK_ERR_PROTO;
		if (r != PW_OK)
			break;
		for (int i = 0; i < n; i++) {
			const uint8_t c = buf[i];
			if (c == '\n') {
				if (finish_para(d, line_start, 1)) { r = PW_ERR_TOO_BIG; break; }
				line_start = off + (uint32_t)i + 1;
				continue;
			}
			if (lb_len == lb_cap) {
				int cap = lb_cap ? lb_cap * 2 : 1024;
				uint8_t *nb = realloc(lb, (size_t)cap);
				if (!nb) { r = PW_ERR_TOO_BIG; break; }
				lb = nb;
				lb_cap = cap;
			}
			lb[lb_len++] = c;
		}
		if (r != PW_OK)
			break;
		off += (uint32_t)n;
		if (prog)
			prog(off, size);
	}
	if (r == PW_OK && finish_para(d, line_start, 0))
		r = PW_ERR_TOO_BIG;
	if (lb_cap > 4096) {           /* don't keep a huge line buffer around */
		free(lb);
		lb = 0;
		lb_cap = 0;
	}
	if (r != PW_OK) {
		doc_clear(d);
		d->name[0] = 0;
	}
	return r;
}

int doc_create(doc_t *d, const uint8_t *name, int nlen) {
	int r = link_call(PW_OP_CREATE, name, nlen, buf, sizeof buf, 0, T_CREATE);
	if (r != PW_OK)
		return r;
	doc_clear(d);
	memcpy(d->name, name, (size_t)nlen);
	d->name[nlen] = 0;
	return PW_OK;
}

/* ======================================================================
 * save
 * ====================================================================== */

/* the SAVE_DATA request being filled: [chunk u16][ops...] */
static struct {
	uint8_t  req[2 + PW_SAVE_CHUNK];
	int      len;            /* bytes of ops in req */
	int      bytes_at;       /* index of the open BYTES op, or -1 */
	uint16_t chunk;
	int      have_keep;
	uint32_t koff, klen;
	uint32_t size, crc;
	int      err;
} sv;

#define SMALL_KEEP 16        /* shorter kept pieces are cheaper sent as text */

/* Collected requests, sent after the document has been gone through. If
 * the changes are bigger than this, they are sent while going through the
 * document instead (no typing during that save). */
#define SNAP_MAX   48        /* x 1.9 KB */
#define SNAP_FULL  (-200)
static uint8_t *snap[SNAP_MAX];
static int      snap_len[SNAP_MAX], snap_n, snapping;

static void snap_free(void) {
	for (int i = 0; i < snap_n; i++)
		free(snap[i]);
	snap_n = 0;
}

static void sv_flush_chunk(void) {
	if (sv.err || sv.len == 0)
		return;
	pw_put16(sv.req, sv.chunk);
	if (snapping) {
		uint8_t *c = (snap_n < SNAP_MAX) ? malloc((size_t)(2 + sv.len)) : 0;
		if (!c) {
			sv.err = SNAP_FULL;
			return;
		}
		memcpy(c, sv.req, (size_t)(2 + sv.len));
		snap[snap_n] = c;
		snap_len[snap_n++] = 2 + sv.len;
	} else {
		int r = link_call(PW_OP_SAVE_DATA, sv.req, 2 + sv.len, buf, sizeof buf, 0, T_SAVE);
		if (r != PW_OK)
			sv.err = r;
	}
	doc_save_sent += (uint32_t)sv.len;
	sv.chunk++;
	sv.len = 0;
	sv.bytes_at = -1;
}

static void sv_flush_keep(void) {
	if (!sv.have_keep)
		return;
	if (sv.len + 9 > PW_SAVE_CHUNK)
		sv_flush_chunk();
	uint8_t *o = sv.req + 2 + sv.len;
	o[0] = PW_SAVE_KEEP;
	pw_put32(o + 1, sv.koff);
	pw_put32(o + 5, sv.klen);
	sv.len += 9;
	sv.bytes_at = -1;
	sv.have_keep = 0;
}

static void sv_bytes(const uint8_t *s, int n) {
	sv_flush_keep();
	while (n > 0 && !sv.err) {
		if (sv.bytes_at < 0) {
			if (sv.len + 4 > PW_SAVE_CHUNK)
				sv_flush_chunk();
			sv.bytes_at = 2 + sv.len;
			sv.req[sv.bytes_at] = PW_SAVE_BYTES;
			pw_put16(sv.req + sv.bytes_at + 1, 0);
			sv.len += 3;
		}
		int room = PW_SAVE_CHUNK - sv.len;
		int k = n < room ? n : room;
		memcpy(sv.req + 2 + sv.len, s, (size_t)k);
		sv.len += k;
		pw_put16(sv.req + sv.bytes_at + 1,
		         (uint16_t)(pw_get16(sv.req + sv.bytes_at + 1) + k));
		s += k;
		n -= k;
		if (n > 0)
			sv_flush_chunk();
	}
}

/* a piece of the new file: s = its bytes; keep_off >= 0 if the working
 * copy has the same bytes there */
static void sv_piece(const uint8_t *s, int n, int32_t keep_off) {
	if (n <= 0)
		return;
	sv.size += (uint32_t)n;
	sv.crc = pw_crc32(sv.crc, s, (size_t)n);
	if (keep_off >= 0 && sv.have_keep && sv.koff + sv.klen == (uint32_t)keep_off) {
		sv.klen += (uint32_t)n;
		return;
	}
	if (keep_off >= 0 && n >= SMALL_KEEP) {
		sv_flush_keep();
		sv.have_keep = 1;
		sv.koff = (uint32_t)keep_off;
		sv.klen = (uint32_t)n;
		return;
	}
	sv_bytes(s, n);
}

/* go through the document: build (or send) the SAVE_DATA requests and note
 * every paragraph's place in the new working copy */
static void emit(doc_t *d, int full, uint32_t gen, doc_progress_fn prog) {
	static const uint8_t nl = '\n';
	memset(&sv, 0, sizeof sv);
	sv.bytes_at = -1;
	doc_save_sent = 0;
	for (int i = 0; i < d->n && !sv.err; i++) {
		para_t *p = &d->p[i];
		const int keep = !full && p->clean;
		p->sgen = gen;
		p->nooff = sv.size;
		p->nonl = (uint8_t)(i + 1 < d->n);
		sv_piece(p->t, p->len, keep ? (int32_t)p->ooff : -1);
		if (p->nonl)
			sv_piece(&nl, 1, (keep && p->onl) ? (int32_t)(p->ooff + (uint32_t)p->olen) : -1);
		if (prog && !snapping && (i & 63) == 0)
			prog((uint32_t)i, (uint32_t)d->n);
	}
	sv_flush_keep();
	sv_flush_chunk();
}

static int save_once(doc_t *d, int full, doc_progress_fn prog, void (*idle)(void)) {
	int r, n;
	uint32_t gen = ++d->save_gen;
	if (gen == 0)
		gen = d->save_gen = 1;

	link_wait_hook = idle;           /* typing now still ends up in this save */
	r = link_call(PW_OP_SAVE_BEGIN, 0, 0, buf, sizeof buf, 0, T_SAVE);
	link_wait_hook = 0;
	if (r != PW_OK)
		return r;

	/* normally: collect the changes, then send them with typing allowed */
	snapping = !full && idle;
	emit(d, full, gen, prog);
	if (sv.err == SNAP_FULL) {
		snap_free();
		snapping = 0;
		emit(d, full, gen, prog);   /* too much to collect: send as we go */
	}
	if (sv.err) {
		snap_free();
		return sv.err;
	}
	const uint32_t size = sv.size, crc = sv.crc;
	const uint32_t edits_before = d->edit_gen;   /* the text being saved */

	if (snapping)
		link_wait_hook = idle;
	r = PW_OK;
	for (int i = 0; i < snap_n && r == PW_OK; i++) {
		r = link_call(PW_OP_SAVE_DATA, snap[i], snap_len[i], buf, sizeof buf, 0, T_SAVE);
		if (prog && snap_n > 1)
			prog((uint32_t)i + 1, (uint32_t)snap_n);
	}
	snap_free();
	if (r == PW_OK) {
		uint8_t args[8];
		pw_put32(args, size);
		pw_put32(args + 4, crc);
		r = link_call(PW_OP_SAVE_END, args, 8, buf, sizeof buf, &n, T_SAVE_END);
	}
	link_wait_hook = 0;
	snapping = 0;
	if (r != PW_OK)
		return r;

	/* the saved text is the new working copy; paragraphs edited while it
	 * was being written are simply unsaved changes */
	for (int i = 0; i < d->n; i++) {
		para_t *p = &d->p[i];
		if (p->sgen == gen) {
			p->ooff = p->nooff;
			p->olen = p->len;
			p->onl = p->nonl;
			p->clean = 1;
		} else {
			p->clean = 0;
		}
	}
	d->modified = (d->edit_gen != edits_before);
	return PW_OK;
}

int doc_save(doc_t *d, doc_progress_fn prog, void (*idle)(void)) {
	doc_save_full = 0;
	int r = save_once(d, 0, prog, idle);
	if (r == PW_ERR_VERIFY) {
		/* the Pico's copy differs from what we think it is: send it all */
		doc_save_full = 1;
		r = save_once(d, 1, prog, 0);
	} else if (r == PW_ERR_BAD_STATE && d->name[0]) {
		/* the Pico restarted and forgot the open file: open it again
		 * (that rebuilds its working copy) and send everything */
		int n;
		r = link_call(PW_OP_OPEN, d->name, (int)strlen((const char *)d->name),
		              buf, sizeof buf, &n, T_OPEN);
		if (r == PW_OK) {
			doc_save_full = 1;
			r = save_once(d, 1, prog, 0);
		}
	}
	return r;
}
