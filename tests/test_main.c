/*
 * test_main.c - PC tests: the real PS1 editor talking to the real Pico file
 * server, AI link and bus logic, with a folder on the PC as the SD card and
 * a pretend PC on the AI link (aiport_fake.c).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "editor.h"
#include "link.h"
#include "doc.h"
#include "shared.h"
#include "fileserver.h"
#include "keymap.h"
#include "charset.h"
#include "ai.h"
#include "vfs.h"

void vfs_posix_root(const char *dir);
void vfs_posix_set_ready(int r);
void host_pico_background(void);
extern int  fake_pc_on;
extern char fake_pc_doc[], fake_pc_question[];
extern long fake_pc_doc_len;
extern unsigned fake_pc_para, fake_pc_off;
extern const char *fake_pc_extra, *fake_pc_answer;
extern uint32_t host_pico_delay_ms;
extern void (*host_idle_hook)(void);
void host_advance(uint32_t ms);
extern int sio_fault_every;
extern unsigned long sio_faults;

static char sd[256];
static int  fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------- helpers */

static uint8_t *read_file(const char *rel, long *n) {
	char p[512];
	snprintf(p, sizeof p, "%s%s", sd, rel);
	FILE *f = fopen(p, "rb");
	if (!f) { *n = -1; return 0; }
	fseek(f, 0, SEEK_END);
	*n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc((size_t)*n + 1);
	if (fread(b, 1, (size_t)*n, f) != (size_t)*n) *n = -1;
	b[*n > 0 ? *n : 0] = 0;
	fclose(f);
	return b;
}

static void write_file(const char *rel, const void *s, long n) {
	char p[512];
	snprintf(p, sizeof p, "%s%s", sd, rel);
	FILE *f = fopen(p, "wb");
	fwrite(s, 1, (size_t)n, f);
	fclose(f);
}

static int file_exists(const char *rel) {
	char p[512];
	snprintf(p, sizeof p, "%s%s", sd, rel);
	return access(p, F_OK) == 0;
}

/* UTF-8 -> Latin-1 the way the Pico converts (for comparing files on the
 * card with the document) */
static uint8_t *utf8_to_l1(const uint8_t *s, long n, long *out_n) {
	uint8_t *o = malloc((size_t)n * 3 + 1);
	utf8_dec_t d = { 0, 0 };
	long k = 0;
	for (long i = 0; i < n; i++) {
		uint32_t u;
		int r = utf8_decode(&d, s[i], &u);
		if (r == 0 || u == '\r')
			continue;
		int c = r > 0 ? unicode_to_latin1(u) : -1;
		const char *f = (c < 0 && r > 0) ? unicode_fallback(u) : 0;
		if (c >= 0) o[k++] = (uint8_t)c;
		else if (f) while (*f) o[k++] = (uint8_t)*f++;
		else o[k++] = '?';
	}
	*out_n = k;
	return o;
}

/* the document as one file */
static uint8_t *doc_text(const doc_t *d, long *n) {
	long total = 0;
	for (int i = 0; i < d->n; i++)
		total += d->p[i].len + 1;
	uint8_t *b = malloc((size_t)total + 1), *o = b;
	for (int i = 0; i < d->n; i++) {
		if (d->p[i].len)
			memcpy(o, d->p[i].t, (size_t)d->p[i].len);
		o += d->p[i].len;
		if (i + 1 < d->n) *o++ = '\n';
	}
	*n = o - b;
	return b;
}

/* compare the document in the editor with the file on the card */
static int doc_matches_card(const char *utf8_name) {
	char rel[256];
	long fn, ln, dn;
	snprintf(rel, sizeof rel, "/ps1writer/%s", utf8_name);
	uint8_t *f = read_file(rel, &fn);
	if (!f) return 0;
	uint8_t *l1 = utf8_to_l1(f, fn, &ln);
	uint8_t *dt = doc_text(&ed_doc, &dn);
	int ok = ln == dn && memcmp(l1, dt, (size_t)dn) == 0;
	if (!ok) {
		long i = 0;
		while (i < ln && i < dn && l1[i] == dt[i]) i++;
		printf("   card %ld bytes, doc %ld bytes, first difference at %ld\n", ln, dn, i);
	}
	free(f); free(l1); free(dt);
	return ok;
}

/* the console's main loop, one frame */
static void frame(void) {
	ed_frame();                 /* the console */
	host_pico_background();     /* the Pico's core 1 */
	fileserver_poll();
	host_advance(16);
}

static void frames(int n) { while (n--) frame(); }

static void push(uint8_t kind, uint8_t code, uint8_t mods) {
	while (!keyq_push(kind, code, mods))
		frame();
}

static void type(const char *latin1) {
	for (const uint8_t *s = (const uint8_t *)latin1; *s; s++) {
		if (*s == '\n') push(PW_EV_KEY, PW_KEY_ENTER, 0);
		else push(PW_EV_CHAR, *s, 0);
	}
	frames(3);
}

static void key(uint8_t code, uint8_t mods) { push(PW_EV_KEY, code, mods); frames(2); }
static void ctrl(char c) { push(PW_EV_CTRL, (uint8_t)c, PW_MOD_CTRL); frames(2); }

/* print the screen (Latin-1 -> UTF-8), for a look by eye */
static void dump_screen(const char *title) {
	printf("---- %s ----\n", title);
	for (int y = 0; y < ed_rows; y++) {
		uint8_t line[ED_COLS * 3 + 4];
		int k = 0;
		for (int x = 0; x < ED_COLS; x++) {
			uint8_t c = ed_screen[y][x].ch;
			if (y == ed_cur_y && x == ed_cur_x) c = '_';
			k += utf8_encode(latin1_to_unicode(c), line + k);
		}
		while (k > 0 && line[k - 1] == ' ') k--;
		line[k] = 0;
		printf("|%s\n", line);
	}
}

/* the raw screen, for tools/preview.py */
static void save_screen(const char *path) {
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fwrite(ed_screen, sizeof ed_screen, 1, f);
	int16_t extra[3] = { (int16_t)ed_cur_x, (int16_t)ed_cur_y, (int16_t)ed_rows };
	fwrite(extra, sizeof extra, 1, f);
	fclose(f);
}

static int doc_word_total(void) {
	int w = 0;
	for (int i = 0; i < ed_doc.n; i++)
		w += doc_count_words(ed_doc.p[i].t, ed_doc.p[i].len);
	return w;
}

static void boot(void) {
	shared_reset();
	pw_flags = PW_FLAG_KEYBOARD | PW_FLAG_SD;
	vfs_posix_root(sd);
	fileserver_init();
	ai_init();
	doc_free(&ed_doc);           /* a second boot in the same process */
	ed_init(0);
}

/* ------------------------------------------------------------------ tests */

/* open a document from the list by its UTF-8 file name (the list is sorted
 * the same way the Pico sorts it) */
static int names_cmp(const void *a, const void *b) {
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void open_doc(const char *utf8name) {
	char cmd[400], buf[256];
	char *names[64];
	int n = 0, idx = -1;
	snprintf(cmd, sizeof cmd, "ls -1 '%s/ps1writer'", sd);
	FILE *p = popen(cmd, "r");
	while (p && n < 64 && fgets(buf, sizeof buf, p)) {
		buf[strcspn(buf, "\n")] = 0;
		char full[600];
		snprintf(full, sizeof full, "%s/ps1writer/%s", sd, buf);
		if (buf[0] == '.' || !strcmp(buf, "backup")) continue;
		names[n++] = strdup(buf);
	}
	if (p) pclose(p);
	qsort(names, (size_t)n, sizeof names[0], names_cmp);
	for (int i = 0; i < n; i++)
		if (!strcmp(names[i], utf8name)) idx = i;
	for (int i = 0; i < n; i++) free(names[i]);
	ctrl('o');
	frames(5);
	key(PW_KEY_HOME, 0);
	for (int i = 0; i < idx; i++) key(PW_KEY_DOWN, 0);
	push(PW_EV_KEY, PW_KEY_ENTER, 0);
	frames(10);
}

static const char sample_utf8[] =
	"# Introduction\r\n"
	"\r\n"
	"This thesis was \xE2\x80\x9Cwritten\xE2\x80\x9D on a PlayStation \xE2\x80\x94 "
	"caf\xC3\xA9, na\xC3\xAFve, \xC5\x9F\xC4\xB1 \xE2\x82\xAC" "5.\r\n"
	"No emoji \xF0\x9F\x98\x80 here.\r\n";

static void test_open_and_convert(void) {
	printf("open, UTF-8 -> Latin-1\n");
	if (system("mkdir -p sd/ps1writer")) return;
	write_file("/ps1writer/thesis.md", sample_utf8, (long)sizeof sample_utf8 - 1);
	boot();
	frames(60);
	/* no last.txt: the editor shows the file list */
	push(PW_EV_KEY, PW_KEY_ENTER, 0);
	frames(10);
	CHECK(ed_doc.n == 5, "paragraphs %d", ed_doc.n);
	CHECK(!strcmp((char *)ed_doc.name, "thesis.md"), "name %s", ed_doc.name);
	if (ed_doc.n == 5) {
		CHECK(ed_doc.p[0].len == 14 && !memcmp(ed_doc.p[0].t, "# Introduction", 14), "heading");
		const char *want2 = "This thesis was \"written\" on a PlayStation - caf\xE9, na\xEFve, si EUR5.";
		CHECK(ed_doc.p[2].len == (int)strlen(want2) && !memcmp(ed_doc.p[2].t, want2, strlen(want2)),
		      "converted text");
		CHECK(ed_doc.p[3].len == 16 && ed_doc.p[3].t[9] == '?', "emoji -> ?");
		CHECK(ed_doc.p[4].len == 0, "last empty paragraph");
	}
	CHECK(ed_doc.words == (uint32_t)doc_word_total(), "word count %u", ed_doc.words);
	dump_screen("opened");
}

static void test_type_and_save(void) {
	printf("typing, patch save, backups\n");
	key(PW_KEY_END, PW_MOD_CTRL);
	type("New paragraph: caf\xE9, na\xEFve, Gr\xF6\xDF" "e.\n");
	ctrl('s');
	frames(5);
	CHECK(!ed_doc.modified, "saved");
	CHECK(doc_matches_card("thesis.md"), "card == doc");
	CHECK(file_exists("/ps1writer/backup/thesis.md.1"), "backup 1");
	CHECK(doc_save_full == 0, "patch save");
	printf("   sent %u bytes of save data for a %u byte document\n", doc_save_sent, ed_doc.bytes);
	CHECK(doc_save_sent < 120, "small patch (%u)", doc_save_sent);

	long n;
	uint8_t *f = read_file("/ps1writer/thesis.md", &n);
	CHECK(f && strstr((char *)f, "caf\xC3\xA9, na\xC3\xAFve, Gr\xC3\xB6\xC3\x9F" "e.") != 0,
	      "UTF-8 on the card");
	CHECK(f && !strchr((char *)f, '\r'), "no CR");
	free(f);

	/* 6 more saves: backups 1..5 and no 6 */
	for (int i = 0; i < 6; i++) {
		type("x");
		ctrl('s');
	}
	CHECK(file_exists("/ps1writer/backup/thesis.md.5") && !file_exists("/ps1writer/backup/thesis.md.6"),
	      "5 backups");
	CHECK(doc_matches_card("thesis.md"), "card == doc after more saves");
}

static void test_reopen_last(void) {
	printf("restart: last document reopens\n");
	boot();
	frames(80);
	CHECK(!strcmp((char *)ed_doc.name, "thesis.md") && ed_doc.n > 5, "reopened %s", ed_doc.name);
	CHECK(doc_matches_card("thesis.md"), "same text");
}

/* random editing with a save every so often; the card must always equal
 * the document */
static void fuzz(int rounds, int edits, const char *label) {
	printf("%s\n", label);
	static const char *words[] = { "thesis ", "caf\xE9 ", "na\xEFve ", "a",
	                               "a longer word ", "\xC4\xD6\xDC\xDF ", ". " };
	int full_saves = 0;
	for (int r = 0; r < rounds; r++) {
		for (int e = 0; e < edits; e++) {
			int op = rand() % 20;
			if (op < 7) type(words[rand() % 7]);
			else if (op < 8) key(PW_KEY_ENTER, 0);
			else if (op < 10) key(PW_KEY_BACKSPACE, 0);
			else if (op < 11) key(PW_KEY_DELETE, 0);
			else if (op < 13) key(PW_KEY_UP, 0);
			else if (op < 15) key(PW_KEY_DOWN, 0);
			else if (op < 16) key(PW_KEY_LEFT, PW_MOD_CTRL);
			else if (op < 17) key(PW_KEY_RIGHT, 0);
			else if (op < 18) key(PW_KEY_PGDN, 0);
			else if (op < 19) key(PW_KEY_PGUP, 0);
			else key(PW_KEY_END, 0);
		}
		ctrl('s');
		full_saves += doc_save_full;
		CHECK(!ed_doc.modified, "%s: round %d saved", label, r);
		if (!doc_matches_card((const char *)ed_doc.name)) {
			CHECK(0, "%s: round %d card != doc", label, r);
			return;
		}
		CHECK(ed_doc.words == (uint32_t)doc_word_total(), "word count");
	}
	CHECK(full_saves == 0, "%s: %d full saves", label, full_saves);
}

static void test_big_document(void) {
	printf("300 KB document\n");
	/* build a thesis-sized UTF-8 file */
	long cap = 400000, n = 0;
	char *b = malloc((size_t)cap);
	for (int para = 0; n < 300000; para++) {
		if (para % 40 == 0)
			n += sprintf(b + n, "## Chapter %d\n\n", para / 40 + 1);
		int words = 30 + rand() % 120;
		for (int w = 0; w < words; w++)
			n += sprintf(b + n, "%s ", (const char *[]){ "data", "caf\xC3\xA9", "results",
			             "na\xC3\xAFve", "method", "analysis" }[rand() % 6]);
		n += sprintf(b + n, "\n\n");
	}
	write_file("/ps1writer/big.md", b, n);
	free(b);

	open_doc("big.md");
	CHECK(!strcmp((char *)ed_doc.name, "big.md"), "opened %s", ed_doc.name);
	printf("   %u bytes, %d paragraphs, %u words\n", ed_doc.bytes, ed_doc.n, ed_doc.words);
	CHECK(doc_matches_card("big.md"), "loaded == card");

	/* edit in the middle and save: the patch must stay small */
	for (int i = 0; i < 40; i++) key(PW_KEY_PGDN, 0);
	type("A sentence added in the middle. ");
	ctrl('s');
	printf("   sent %u bytes of save data\n", doc_save_sent);
	CHECK(doc_save_sent < 2000, "patch %u", doc_save_sent);   /* the changed paragraph + a few KEEPs */
	CHECK(doc_matches_card("big.md"), "card == doc");
	dump_screen("big document, middle");
	save_screen("screen_edit.bin");
	key(PW_KEY_F1, 0);
	save_screen("screen_help.bin");
	key(PW_KEY_ESC, 0);
	ctrl('o');
	save_screen("screen_open.bin");
	key(PW_KEY_ESC, 0);
}

static void test_verify_and_restart(void) {
	printf("damaged working copy -> full save; Pico restart -> reopen\n");
	type("one more change");
	/* damage the Pico's working copy behind its back */
	long n;
	uint8_t *w = read_file("/ps1writer/.work/doc.latin1", &n);
	CHECK(w != 0, "working copy exists");
	if (!w) return;
	w[n / 2] ^= 0x20;
	write_file("/ps1writer/.work/doc.latin1", w, n);
	free(w);
	ctrl('s');
	CHECK(doc_save_full == 1 && !ed_doc.modified, "full save after verify failure");
	CHECK(doc_matches_card("big.md"), "card == doc");

	type(" and another one");
	fileserver_init();           /* the Pico rebooted: it forgot the open file */
	ctrl('s');
	CHECK(doc_save_full == 1 && !ed_doc.modified, "saved after Pico restart");
	CHECK(doc_matches_card("big.md"), "card == doc");
}

static void test_new_document(void) {
	printf("new document with an accented name\n");
	ctrl('n');
	type("Kapitel Gr\xF6\xDF" "e");
	push(PW_EV_KEY, PW_KEY_ENTER, 0);
	frames(5);
	CHECK(!strcmp((char *)ed_doc.name, "Kapitel Gr\xF6\xDF" "e.md"), "name %s", ed_doc.name);
	CHECK(file_exists("/ps1writer/Kapitel Gr\xC3\xB6\xC3\x9F" "e.md"), "UTF-8 file name");
	type("# Heading\n\nText.");
	ctrl('s');
	CHECK(doc_matches_card("Kapitel Gr\xC3\xB6\xC3\x9F" "e.md"), "card == doc");
	ctrl('n');
	type("thesis.md");
	push(PW_EV_KEY, PW_KEY_ENTER, 0);
	frames(5);
	CHECK(strcmp((char *)ed_doc.name, "thesis.md") != 0, "existing name refused");
	key(PW_KEY_ESC, 0);
	dump_screen("new document");
}

static void test_name_lookup(void) {
	printf("a file name Latin-1 can't show (made on a PC)\n");
	const char *name = "B\xC3\xB6l\xC3\xBCm \xC5\x9E" "ahin.md";      /* Bölüm Şahin.md */
	write_file("/ps1writer/B\xC3\xB6l\xC3\xBCm \xC5\x9E" "ahin.md", "PC text\n", 8);
	open_doc(name);
	CHECK(!strcmp((char *)ed_doc.name, "B\xF6l\xFCm Sahin.md"), "shown as %s", ed_doc.name);
	CHECK(ed_doc.n == 2 && ed_doc.p[0].len == 7, "opened the right file");
	type("more ");
	ctrl('s');
	CHECK(doc_matches_card(name), "saved to the original file");
	CHECK(!file_exists("/ps1writer/B\xC3\xB6l\xC3\xBCm Sahin.md"), "no new file with the shown name");
}

static void test_wrap_and_cursor(void) {
	printf("wrapping and cursor movement\n");
	ctrl('n');
	type("wrap");
	push(PW_EV_KEY, PW_KEY_ENTER, 0);
	frames(5);
	char line[2000];
	int n = 0;
	for (int i = 0; i < 60; i++) n += sprintf(line + n, "word%d ", i);
	n += sprintf(line + n, "%s", "X");
	for (int i = 0; i < 200; i++) line[n++] = 'y';   /* a 200-letter word */
	line[n] = 0;
	type(line);
	type("\nshort line");
	frames(2);
	/* every screen row fits and only breaks after a space (or in the long word) */
	int bad = 0;
	for (int y = 0; y < ed_rows - 1; y++) {
		int last = ED_COLS - 1;
		while (last >= 0 && ed_screen[y][last].ch == ' ') last--;
		if (last == ED_COLS - 1 && ed_screen[y][last].ch != 'y') bad++;
	}
	CHECK(bad == 0, "rows wrap at spaces (%d bad)", bad);

	/* down through everything then up again returns to the same place */
	key(PW_KEY_HOME, PW_MOD_CTRL);
	for (int i = 0; i < 50; i++) key(PW_KEY_DOWN, 0);
	for (int i = 0; i < 50; i++) key(PW_KEY_UP, 0);
	CHECK(ed_cur_x == 0 && ed_cur_y == 0, "back to the top (%d,%d)", ed_cur_x, ed_cur_y);
	dump_screen("wrapping");
	ctrl('s');
	CHECK(doc_matches_card("wrap.md"), "card == doc");
}

static int cursor_on(const char *s) {
	if (ed_cur_y < 0) return 0;
	for (int i = 0; s[i]; i++)
		if (ed_cur_x + i >= ED_COLS || ed_screen[ed_cur_y][ed_cur_x + i].ch != (uint8_t)s[i])
			return 0;
	return 1;
}

static void test_find(void) {
	printf("find\n");
	key(PW_KEY_HOME, PW_MOD_CTRL);
	ctrl('f');
	type("SHORT");                     /* ignores case */
	push(PW_EV_KEY, PW_KEY_ENTER, 0);
	frames(3);
	CHECK(cursor_on("short line"), "found");
	ctrl('g');                         /* only one match: wraps to itself */
	CHECK(cursor_on("short line"), "find again");
}

static void test_autosave(void) {
	printf("autosave\n");
	type(" automatic");
	CHECK(ed_doc.modified, "modified");
	frames(200);                       /* 3 s idle, but the last save is recent */
	CHECK(ed_doc.modified, "no autosave yet");
	host_advance(60000);
	frames(5);
	CHECK(!ed_doc.modified, "autosaved");
	CHECK(doc_matches_card((const char *)ed_doc.name), "card == doc");
}

/* someone typing while the (slow) Pico writes the card */
static int typed_during;
static void typist(void) {
	if (plat_ms() % 100 == 0 && typed_during < 30) {   /* 3 s of typing */
		keyq_push(PW_EV_CHAR, 'k', 0);
		if (++typed_during == 10) {
			keyq_push(PW_EV_CTRL, 's', PW_MOD_CTRL);   /* refused while saving */
			keyq_push(PW_EV_KEY, PW_KEY_ENTER, 0);
		}
	}
}

static void test_typing_while_saving(void) {
	printf("typing while the card is written\n");
	key(PW_KEY_END, PW_MOD_CTRL);
	type("\nWhile saving: ");
	host_pico_delay_ms = 1500;
	typed_during = 0;
	host_idle_hook = typist;
	ctrl('s');
	host_idle_hook = 0;
	host_pico_delay_ms = 0;
	printf("   %d keys typed during the save\n", typed_during);
	CHECK(typed_during >= 20, "keys typed during the save: %d", typed_during);
	frames(30);                        /* the keys still queued */
	int ks = 0;
	for (int i = ed_doc.n - 2; i < ed_doc.n; i++)
		for (int j = 0; j < ed_doc.p[i].len; j++)
			ks += ed_doc.p[i].t[j] == 'k';
	CHECK(ks == typed_during, "all typed keys arrived (%d of %d)", ks, typed_during);
	CHECK(ed_doc.modified, "typed text is unsaved");
	CHECK(!doc_matches_card((const char *)ed_doc.name), "card has the earlier text");
	ctrl('s');
	CHECK(doc_save_full == 0, "next save is still a patch");
	CHECK(!ed_doc.modified && doc_matches_card((const char *)ed_doc.name), "card == doc");
}

static void test_screen_rows(void) {
	printf("screen rows (F11 / F12), kept on the card\n");
	CHECK(ed_rows == ED_DEFAULT_ROWS, "default %d rows", ed_rows);
	key(PW_KEY_F1 + 10, 0);            /* F11: fewer */
	key(PW_KEY_F1 + 10, 0);
	CHECK(ed_rows == ED_DEFAULT_ROWS - 2, "F11 twice -> %d", ed_rows);
	frames(2);
	int status_y = -1;
	for (int y = 0; y < ED_MAX_ROWS; y++)
		if (ed_screen[y][0].attr == A_STATUS) status_y = y;
	CHECK(status_y == ed_rows - 1, "status bar on the last row (%d)", status_y);
	boot();
	frames(80);
	CHECK(ed_rows == ED_DEFAULT_ROWS - 2, "restored after restart: %d", ed_rows);
	for (int i = 0; i < 10; i++) key(PW_KEY_F1 + 11, 0);   /* F12: more, up to 30 */
	CHECK(ed_rows == ED_MAX_ROWS, "at most %d: %d", ED_MAX_ROWS, ed_rows);
	for (int i = 0; i < 2; i++) key(PW_KEY_F1 + 10, 0);
	CHECK(ed_rows == ED_DEFAULT_ROWS, "back to %d", ed_rows);
}

static int doc_contains(const char *s) {
	long n;
	uint8_t *t = doc_text(&ed_doc, &n);
	int found = 0;
	const long m = (long)strlen(s);
	for (long i = 0; i + m <= n && !found; i++)
		found = !memcmp(t + i, s, (size_t)m);
	free(t);
	return found;
}

static int chat_screen_contains(const char *s) {
	for (int y = 0; y < ed_rows; y++) {
		char row[ED_COLS + 1];
		for (int x = 0; x < ED_COLS; x++) row[x] = (char)ed_screen[y][x].ch;
		row[ED_COLS] = 0;
		if (strstr(row, s)) return 1;
	}
	return 0;
}

static void test_ai(void) {
	printf("AI chat over the (pretend) PC link\n");
	open_doc("thesis.md");
	key(PW_KEY_END, PW_MOD_CTRL);
	type("\nUnsaved line before asking.");

	fake_pc_on = 0;
	frames(5);
	key(PW_KEY_F1 + 1, 0);             /* F2: chat */
	type("Hello?");
	push(PW_EV_KEY, PW_KEY_ENTER, 0);
	frames(5);
	CHECK(fake_pc_question[0] == 0, "nothing sent without the bridge");

	fake_pc_on = 1;
	fake_pc_extra = " caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D \xC5\x9F";   /* café “quoted” ş */
	frames(10);                        /* HELLO arrives, the AI flag goes up */
	CHECK(pw_flags & PW_FLAG_AI, "AI flag");
	push(PW_EV_KEY, PW_KEY_ENTER, 0);  /* the question is still in the input line */
	frames(30);
	CHECK(!strcmp(fake_pc_question, "Hello?"), "question arrived: '%s'", fake_pc_question);
	CHECK(!ed_doc.modified, "saved before asking");
	long n;
	uint8_t *card = read_file("/ps1writer/thesis.md", &n);
	CHECK(card && n == fake_pc_doc_len && !memcmp(card, fake_pc_doc, (size_t)n),
	      "the PC got the saved document (%ld / %ld bytes)", fake_pc_doc_len, n);
	free(card);
	CHECK(fake_pc_para == (unsigned)(ed_doc.n - 1) && fake_pc_off == 27,
	      "cursor sent: %u:%u", fake_pc_para, fake_pc_off);
	CHECK(chat_screen_contains("AI: Answer to: Hello?"), "answer shown in the chat");
	dump_screen("AI chat");

	/* F4: insert at the cursor */
	key(PW_KEY_F1 + 3, 0);
	CHECK(doc_contains("Unsaved line before asking.Answer to: Hello?\nSecond line. caf\xE9 \"quoted\" s"),
	      "answer inserted at the cursor (Latin-1, with stand-ins)");
	ctrl('s');
	CHECK(doc_matches_card("thesis.md"), "card == doc");

	/* F5 twice: replace everything */
	key(PW_KEY_F1 + 1, 0);
	type("Write it all");
	push(PW_EV_KEY, PW_KEY_ENTER, 0);
	frames(30);
	key(PW_KEY_F1 + 4, 0);
	CHECK(!doc_contains("Introduction"), "one F5 changes nothing yet");
	key(PW_KEY_F1 + 4, 0);
	CHECK(ed_doc.n == 2 && doc_contains("Answer to: Write it all") && !doc_contains("Introduction"),
	      "document replaced (%d paragraphs)", ed_doc.n);
	ctrl('s');
	CHECK(doc_matches_card("thesis.md"), "card == doc");
	CHECK(file_exists("/ps1writer/backup/thesis.md.1"), "old version in the backups");

	/* the bridge goes away while the chat is open */
	key(PW_KEY_F1 + 1, 0);
	fake_pc_on = 0;
	frames(10);
	type("x");
	push(PW_EV_KEY, PW_KEY_ENTER, 0);
	frames(5);
	CHECK(!(pw_flags & PW_FLAG_AI), "AI flag down");
	key(PW_KEY_ESC, 0);
	fake_pc_extra = "";
}

static void test_no_sd(void) {
	printf("start without SD card, card inserted later\n");
	shared_reset();
	pw_flags = PW_FLAG_KEYBOARD;
	vfs_posix_set_ready(0);
	fileserver_init();
	doc_free(&ed_doc);
	ed_init(0);
	frames(150);
	CHECK(ed_doc.name[0] == 0, "nothing open without a card");
	dump_screen("waiting for the SD card");
	vfs_posix_set_ready(1);
	pw_flags |= PW_FLAG_SD;
	frames(150);
	CHECK(ed_doc.name[0] != 0, "last document opened after the card came (%s)", ed_doc.name);
}

/* ---- keymap: HID reports -> events ---- */

static struct { uint8_t kind, code, mods; } kev[64];
static int kev_n;
static int kemit(uint8_t kind, uint8_t code, uint8_t mods) {
	kev[kev_n].kind = kind; kev[kev_n].code = code; kev[kev_n].mods = mods;
	kev_n++;
	return 1;
}

static void hid(uint8_t mods, uint8_t k, uint32_t t) {
	uint8_t r[8] = { mods, 0, k, 0, 0, 0, 0, 0 };
	keymap_report(r, 8, t);
}

static void test_keymap(void) {
	printf("keymap (US)\n");
	keymap_init(kemit);
	struct { uint8_t mods, key, kind, code; } t[] = {
		{ 0,    0x0C, PW_EV_CHAR, 'i'  },
		{ 0x02, 0x0C, PW_EV_CHAR, 'I'  },
		{ 0,    0x34, PW_EV_CHAR, '\'' },
		{ 0x20, 0x34, PW_EV_CHAR, '"'  },   /* right shift */
		{ 0,    0x2F, PW_EV_CHAR, '['  },
		{ 0x02, 0x1F, PW_EV_CHAR, '@'  },
		{ 0,    0x36, PW_EV_CHAR, ','  },
		{ 0,    0x59, PW_EV_CHAR, '1'  },   /* keypad */
		{ 0x01, 0x16, PW_EV_CTRL, 's'  },   /* Ctrl+S */
		{ 0,    0x52, PW_EV_KEY,  PW_KEY_UP },
		{ 0,    0x45, PW_EV_KEY,  PW_KEY_F1 + 11 },
	};
	uint32_t now = 1000;
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
		kev_n = 0;
		hid(t[i].mods, t[i].key, now);
		hid(0, 0, now + 50);
		now += 100;
		CHECK(kev_n == 1 && kev[0].kind == t[i].kind && kev[0].code == t[i].code,
		      "key %02X mods %02X -> n=%d kind %d code %02X", t[i].key, t[i].mods,
		      kev_n, kev_n ? kev[0].kind : 0, kev_n ? kev[0].code : 0);
	}
	/* caps lock */
	kev_n = 0;
	hid(0, 0x39, now); hid(0, 0, now + 10);
	hid(0, 0x04, now + 20); hid(0, 0, now + 30);
	hid(0, 0x39, now + 40); hid(0, 0, now + 50);
	CHECK(kev_n == 1 && kev[0].code == 'A', "caps lock");
	now += 100;
	/* repeat: held 1 s -> 1 + about (1000-500)/33 events */
	kev_n = 0;
	hid(0, 0x04, now);
	for (uint32_t ms = 0; ms <= 1000; ms += 5)
		keymap_tick(now + ms);
	hid(0, 0, now + 1001);
	CHECK(kev_n >= 14 && kev_n <= 18, "repeat count %d", kev_n);
}

/* ---- screenshots for the README ---- */

static void demo_screens(void) {
	printf("demo screens\n");
	static const char demo[] =
		"# 2. Related work\n\n"
		"Text editors on game consoles have a short history. Early attempts relied on "
		"on-screen keyboards, which made writing anything longer than a name tedious. "
		"This chapter reviews the hardware that made keyboard input possible and the "
		"software that followed.\n\n"
		"## 2.1 Input devices\n\n"
		"The PlayStation controller port speaks a simple synchronous serial protocol. "
		"Every device answers to an address byte, which lets a microcontroller pose as "
		"a new kind of peripheral without any change to the console.\n\n"
		"## 2.2 Storage\n\n"
		"Memory cards hold 128 KB, far too little for a thesis. An SD card behind the "
		"same port removes that limit.\n";
	write_file("/ps1writer/demo.md", demo, (long)sizeof demo - 1);
	fake_pc_on = 1;
	boot();
	frames(80);
	open_doc("demo.md");
	key(PW_KEY_HOME, PW_MOD_CTRL);
	for (int i = 0; i < 4; i++) key(PW_KEY_DOWN, 0);
	key(PW_KEY_END, 0);
	frames(300);                       /* let the status message time out */
	save_screen("screen_demo.bin");
	fake_pc_answer = "Unlike memory cards, an SD card offers gigabytes of storage and can be read "
	                 "on any PC, so the finished chapters move to a word processor without "
	                 "conversion.";
	key(PW_KEY_F1 + 1, 0);
	type("Write one sentence for 2.2 about why SD cards help.");
	push(PW_EV_KEY, PW_KEY_ENTER, 0);
	frames(40);
	save_screen("screen_chat.bin");
	fake_pc_answer = 0;
	key(PW_KEY_ESC, 0);
}

/* ---- robustness: corrupted bytes on the bus ---- */

static void test_faults(void) {
	sio_fault_every = 300;
	sio_faults = 0;
	fuzz(6, 30, "random edits with bus errors");
	/* a 2000-byte READ answer at 1 bad byte in 300 would almost never get
	 * through; check the load at a (still very bad) 1 in 20000 */
	sio_fault_every = 20000;
	open_doc("big.md");
	CHECK(!strcmp((char *)ed_doc.name, "big.md"), "reopened with bus errors");
	CHECK(doc_matches_card("big.md"), "loaded == card with bus errors");
	printf("   %lu corrupted bytes, %u retries\n", sio_faults, link_retries);
	sio_fault_every = 0;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	srand(1234);
	char cwd[200];
	if (!getcwd(cwd, sizeof cwd)) return 1;
	snprintf(sd, sizeof sd, "%s/sd", cwd);
	if (system("rm -rf sd")) return 1;

	test_open_and_convert();
	test_type_and_save();
	test_reopen_last();
	fuzz(20, 40, "random edits, 20 saves");
	test_big_document();
	fuzz(10, 40, "random edits in the big document");
	test_verify_and_restart();
	test_new_document();
	test_name_lookup();
	test_wrap_and_cursor();
	test_find();
	test_autosave();
	test_typing_while_saving();
	test_screen_rows();
	test_ai();
	test_no_sd();
	test_faults();
	test_keymap();
	demo_screens();

	doc_free(&ed_doc);
	printf("\n%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
