/*
 * editor.c - the ps1-writer editor (see editor.h)
 *
 * Paragraphs are soft-wrapped at ED_COLS columns, breaking after the last
 * space that fits. The cursor is (paragraph, byte offset); "rows" are
 * computed on the fly from the paragraph text, so nothing has to be kept in
 * sync when text changes.
 *
 * Text is Latin-1, one byte per character.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "editor.h"

#define AUTOSAVE_IDLE_MS   3000     /* after this long without typing ... */
#define AUTOSAVE_EVERY_MS  60000    /* ... if the last save is this old */
#define MSG_MS             4000
#define TAB_SPACES         4
#define MAX_FILES          128
#define DEFAULT_NAME       "thesis.md"

#define CHAT_H             10       /* chat panel: title, 8 lines, input */
#define CHAT_LINES         (CHAT_H - 2)
#define CHATLOG_MAX        6000     /* chat history kept for display */
#define ANSWER_MAX         (64 * 1024)
#define AI_POLL_MS         100

cell_t ed_screen[ED_MAX_ROWS][ED_COLS];
int    ed_rows = ED_DEFAULT_ROWS;
int    ed_cur_x = -1, ed_cur_y = -1;
int    ed_overwrite;
doc_t  ed_doc;

enum { S_CONNECT, S_EDIT, S_OPEN, S_NAME, S_FIND, S_HELP, S_CHAT };

static void (*present_fn)(void);
static int      state = S_CONNECT, back_state;
static int      have_doc;
static int      cp, co;            /* cursor: paragraph, offset */
static int      goal = -1;         /* wanted column for up/down */
static int      tp, tr;            /* top of the screen: paragraph, row */
static int      redraw = 1;
static uint8_t  flags_now = 0xFF;
static uint32_t last_edit, last_save, next_connect, last_frame;
static int      saving_now;        /* a save is running; typing goes on */

static char     msg[ED_COLS + 1];
static int      msg_warn;
static uint32_t msg_until;

/* dialogs */
static struct { uint8_t name[PW_MAX_NAME + 1]; uint32_t size; } files[MAX_FILES];
static int      nfiles, fsel, ftop;
static uint8_t  input[PW_MAX_NAME + 1];
static int      input_len;
static uint8_t  find_str[PW_MAX_NAME + 1];

/* AI chat */
static char     chatlog[CHATLOG_MAX + 1];
static int      chatlog_len, chat_scroll;
static uint8_t  question[PW_AI_MAX_QUESTION + 1], last_question[PW_AI_MAX_QUESTION + 1];
static int      question_len;
static uint8_t *answer;            /* the last answer, for F4 / F5 */
static int      answer_len, answer_cap, answer_started;
static int      ai_waiting;
static uint32_t ai_last_poll, replace_armed_until;

#define D (&ed_doc)
#define P(i) (&ed_doc.p[i])

static int text_rows(void) {
	return ed_rows - 1 - (state == S_CHAT ? CHAT_H : 0);
}

/* ======================================================================
 * messages
 * ====================================================================== */

static void say(int warn, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void say(int warn, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	msg_warn = warn;
	msg_until = plat_ms() + MSG_MS;
	redraw = 1;
}

static const char *err_text(int r) {
	static char other[24];
	switch (r) {
	case LINK_ERR_TIMEOUT:     return "the Pico does not answer";
	case LINK_ERR_PROTO:       return "garbled answer from the Pico";
	case PW_ERR_NOT_FOUND:     return "file not found";
	case PW_ERR_EXISTS:        return "a file with that name exists";
	case PW_ERR_IO:            return "SD card error";
	case PW_ERR_NO_SD:         return "no SD card";
	case PW_ERR_BAD_ARGS:      return "invalid request or name";
	case PW_ERR_BAD_STATE:     return "no document open";
	case PW_ERR_VERIFY:        return "save could not be verified";
	case PW_ERR_NOT_AVAILABLE: return "not available";
	case PW_ERR_TOO_BIG:       return "document too big or out of memory";
	}
	snprintf(other, sizeof other, "error %d", r);
	return other;
}

static void refresh(void) {
	redraw = 1;
	ed_render();
	if (present_fn)
		present_fn();
}

static const char *prog_label;
static uint32_t prog_last;

static void progress(uint32_t done, uint32_t total) {
	uint32_t now = plat_ms();
	if (now - prog_last < 100 && done < total)
		return;
	prog_last = now;
	int pct = total ? (int)((done / 16u) * 100u / (total / 16u + 1u)) : 100;
	if (pct > 100)
		pct = 100;
	say(0, "%s %d%%", prog_label, pct);
	refresh();
}

/* ======================================================================
 * word wrap, rows and the cursor
 * ====================================================================== */

/* start of the row after the one starting at s in t[0..len), or -1 if it
 * is the last row. Breaks after the last space that fits. */
static int wrap_next(const uint8_t *t, int len, int s, int width) {
	if (len - s <= width - 1)
		return -1;
	for (int i = s + width - 1; i > s; i--)
		if (t[i] == ' ')
			return i + 1;
	return s + width;
}

static int row_next(const para_t *p, int s) {
	return wrap_next(p->t, p->len, s, ED_COLS);
}

static int para_rows(int pi) {
	int s = 0, n = 1;
	while ((s = row_next(P(pi), s)) >= 0)
		n++;
	return n;
}

static int row_start(int pi, int r) {
	int s = 0;
	while (r-- > 0) {
		int nx = row_next(P(pi), s);
		if (nx < 0)
			break;
		s = nx;
	}
	return s;
}

/* row of offset off in paragraph pi; *start = that row's first offset */
static int row_of(int pi, int off, int *start) {
	int s = 0, r = 0, nx;
	while ((nx = row_next(P(pi), s)) >= 0 && off >= nx) {
		s = nx;
		r++;
	}
	if (start)
		*start = s;
	return r;
}

static void put_in_row(int pi, int s, int col) {
	int nx = row_next(P(pi), s);
	int max = nx < 0 ? P(pi)->len : nx - 1;
	cp = pi;
	co = s + col < max ? s + col : max;
}

static void top_back(int k) {
	while (k-- > 0) {
		if (tr > 0)
			tr--;
		else if (tp > 0)
			tr = para_rows(--tp) - 1;
		else
			break;
	}
}

static void top_fwd(int k) {
	while (k-- > 0) {
		if (tr + 1 < para_rows(tp))
			tr++;
		else if (tp + 1 < D->n)
			tp++, tr = 0;
		else
			break;
	}
}

static void ensure_visible(void) {
	const int rows_on_screen = text_rows();
	if (tp >= D->n)
		tp = D->n - 1, tr = 0;
	int rows = para_rows(tp);
	if (tr >= rows)
		tr = rows - 1;

	int cr = row_of(cp, co, 0);
	if (cp < tp || (cp == tp && cr < tr)) {
		tp = cp;
		tr = cr;
		return;
	}
	int count = 0, pi = tp, r = tr;
	while (pi < cp && count < rows_on_screen) {
		count += para_rows(pi) - r;
		r = 0;
		pi++;
	}
	if (pi == cp)
		count += cr - r;
	if (count >= rows_on_screen) {
		tp = cp;
		tr = cr;
		top_back(rows_on_screen - 1);
	}
}

static int is_space(uint8_t c) { return c == ' ' || c == '\t'; }

/* ---- movement ---- */

static void mv_left(void) {
	if (co > 0) co--;
	else if (cp > 0) cp--, co = P(cp)->len;
}

static void mv_right(void) {
	if (co < P(cp)->len) co++;
	else if (cp + 1 < D->n) cp++, co = 0;
}

static void mv_up(void) {
	int s, r = row_of(cp, co, &s);
	if (goal < 0) goal = co - s;
	if (r > 0)
		put_in_row(cp, row_start(cp, r - 1), goal);
	else if (cp > 0)
		put_in_row(cp - 1, row_start(cp - 1, para_rows(cp - 1) - 1), goal);
}

static void mv_down(void) {
	int s;
	row_of(cp, co, &s);
	if (goal < 0) goal = co - s;
	int nx = row_next(P(cp), s);
	if (nx >= 0)
		put_in_row(cp, nx, goal);
	else if (cp + 1 < D->n)
		put_in_row(cp + 1, 0, goal);
}

static void mv_word_left(void) {
	const uint8_t *t = P(cp)->t;
	if (co == 0) { mv_left(); return; }
	while (co > 0 && is_space(t[co - 1])) co--;
	while (co > 0 && !is_space(t[co - 1])) co--;
}

static void mv_word_right(void) {
	const para_t *p = P(cp);
	if (co == p->len) { mv_right(); return; }
	while (co < p->len && !is_space(p->t[co])) co++;
	while (co < p->len && is_space(p->t[co])) co++;
}

/* ======================================================================
 * editing
 * ====================================================================== */

static void edited(int ok) {
	if (!ok)
		say(1, "Out of memory or document too big (1 MB at most)");
	last_edit = plat_ms();
	goal = -1;
}

static void ins_char(uint8_t c) {
	if (ed_overwrite && co < P(cp)->len)
		doc_erase(D, cp, co, 1);
	int ok = doc_insert(D, cp, co, &c, 1) == 0;
	if (ok) co++;
	edited(ok);
}

static void key_enter(void) {
	int ok = doc_split(D, cp, co) == 0;
	if (ok) cp++, co = 0;
	edited(ok);
}

static void key_backspace(int word) {
	if (co > 0) {
		int to = co;
		if (word)
			mv_word_left();          /* co > 0: stays in this paragraph */
		else
			co--;
		doc_erase(D, cp, co, to - co);
		edited(1);
	} else if (cp > 0) {
		int l = P(cp - 1)->len;
		int ok = doc_join(D, cp - 1) == 0;
		if (ok) cp--, co = l;
		edited(ok);
	}
}

static void key_delete(void) {
	if (co < P(cp)->len) {
		doc_erase(D, cp, co, 1);
		edited(1);
	} else if (cp + 1 < D->n) {
		edited(doc_join(D, cp) == 0);
	}
}

/* insert text (may contain newlines) at the cursor; returns 0 or -1 */
static int insert_text(const uint8_t *s, int n) {
	int i = 0;
	while (i < n) {
		if (s[i] == '\n') {
			if (doc_split(D, cp, co))
				return -1;
			cp++, co = 0;
			i++;
			continue;
		}
		/* a run of up to 256 characters; control characters become spaces */
		uint8_t run[256];
		int k = 0;
		while (i < n && s[i] != '\n' && k < (int)sizeof run) {
			run[k++] = s[i] < 0x20 ? ' ' : s[i];
			i++;
		}
		if (doc_insert(D, cp, co, run, k))
			return -1;
		co += k;
	}
	return 0;
}

/* ======================================================================
 * find (ignores case)
 * ====================================================================== */

static uint8_t fold(uint8_t c) {
	if (c >= 'A' && c <= 'Z') return (uint8_t)(c + 32);
	if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (uint8_t)(c + 0x20);
	return c;
}

static int match_at(const para_t *p, int i, const uint8_t *q, int m) {
	for (int j = 0; j < m; j++)
		if (fold(p->t[i + j]) != fold(q[j]))
			return 0;
	return 1;
}

static void find_next(void) {
	const int m = (int)strlen((const char *)find_str);
	if (m == 0)
		return;
	for (int k = 0; k <= D->n; k++) {
		const int pi = (cp + k) % D->n;
		const para_t *p = P(pi);
		for (int i = (k == 0) ? co + 1 : 0; i + m <= p->len; i++) {
			if (match_at(p, i, find_str, m)) {
				if (k > 0 && pi <= cp)
					say(0, "Search wrapped to the start");
				cp = pi;
				co = i;
				goal = -1;
				return;
			}
		}
	}
	say(1, "Not found: %s", (const char *)find_str);
}

/* ======================================================================
 * settings (kept on the SD card by the Pico)
 * ====================================================================== */

#define SETTINGS_VERSION 1

static void set_rows(int rows) {
	if (rows < ED_MIN_ROWS) rows = ED_MIN_ROWS;
	if (rows > ED_MAX_ROWS) rows = ED_MAX_ROWS;
	ed_rows = rows;
	redraw = 1;
}

static void load_settings(void) {
	uint8_t buf[PW_CONFIG_MAX];
	int n = 0;
	if (link_call(PW_OP_CONFIG_GET, 0, 0, buf, sizeof buf, &n, 2000) == PW_OK &&
	    n >= 2 && buf[0] == SETTINGS_VERSION)
		set_rows(buf[1]);
}

static void save_settings(void) {
	uint8_t buf[2] = { SETTINGS_VERSION, (uint8_t)ed_rows };
	link_call(PW_OP_CONFIG_SET, buf, sizeof buf, 0, 0, 0, 2000);
}

/* ======================================================================
 * files
 * ====================================================================== */

static void reset_view(void) {
	cp = co = tp = tr = 0;
	goal = -1;
	last_save = last_edit = plat_ms();
}

/* called while the Pico writes the card: keep the screen and keyboard alive */
static void save_idle(void) {
	static int inside;
	if (inside || plat_ms() - last_frame < 16)
		return;
	inside = 1;
	ed_frame();
	inside = 0;
}

/* typing: 1 = the user may keep typing while the card is written */
static int do_save(int automatic, int typing) {
	if (!have_doc)
		return PW_ERR_BAD_STATE;
	prog_label = automatic ? "Autosaving" : "Saving";
	prog_last = 0;
	say(0, "%s...", prog_label);
	refresh();
	saving_now = 1;
	int r = doc_save(D, progress, typing ? save_idle : 0);
	saving_now = 0;
	last_save = plat_ms();
	redraw = 1;
	if (r == PW_OK)
		say(0, "%s (%lu bytes)", automatic ? "Autosaved" : "Saved", (unsigned long)D->bytes);
	else
		say(1, "NOT SAVED: %s", err_text(r));
	return r;
}

/* save first if needed; returns 1 if it's fine to leave the document */
static int leave_doc(void) {
	if (!have_doc || !D->modified)
		return 1;
	return do_save(0, 0) == PW_OK;    /* no typing: the document is about to go */
}

static int load_list(void) {
	uint8_t args[2], buf[PW_MAX_PAYLOAD];
	int n;
	nfiles = 0;
	for (;;) {
		pw_put16(args, (uint16_t)nfiles);
		int r = link_call(PW_OP_LIST, args, 2, buf, sizeof buf, &n, 3000);
		if (r != PW_OK)
			return r;
		if (n < 3)
			return LINK_ERR_PROTO;
		const int total = pw_get16(buf), count = buf[2];
		int o = 3;
		for (int i = 0; i < count && nfiles < MAX_FILES; i++) {
			int nl = buf[o];
			if (nl > PW_MAX_NAME || o + 1 + nl + 4 > n)
				return LINK_ERR_PROTO;
			memcpy(files[nfiles].name, buf + o + 1, (size_t)nl);
			files[nfiles].name[nl] = 0;
			files[nfiles].size = pw_get32(buf + o + 1 + nl);
			nfiles++;
			o += 1 + nl + 4;
		}
		if (count == 0 || nfiles >= total || nfiles >= MAX_FILES)
			return PW_OK;
	}
}

static void start_name_dialog(const char *initial) {
	input_len = (int)strlen(initial);
	memcpy(input, initial, (size_t)input_len + 1);
	back_state = state == S_NAME ? back_state : state;
	state = S_NAME;
	redraw = 1;
}

static void start_open_dialog(void) {
	if (!leave_doc())
		return;
	say(0, "Reading the document list...");
	refresh();
	int r = load_list();
	if (r != PW_OK) {
		say(1, "Could not read the list: %s", err_text(r));
		return;
	}
	msg_until = 0;
	if (nfiles == 0) {
		start_name_dialog(DEFAULT_NAME);
		return;
	}
	fsel = 0;
	for (int i = 0; i < nfiles; i++)
		if (have_doc && strcmp((const char *)files[i].name, (const char *)D->name) == 0)
			fsel = i;
	ftop = 0;
	state = S_OPEN;
	redraw = 1;
}

static int open_file(const uint8_t *name, int nlen) {
	prog_label = "Loading";
	prog_last = 0;
	say(0, "Opening...");
	refresh();
	int r = doc_open(D, name, nlen, progress);
	if (r == PW_OK) {
		have_doc = 1;
		reset_view();
		state = S_EDIT;
		say(0, "%s: %lu bytes, %lu words", (const char *)D->name,
		    (unsigned long)D->bytes, (unsigned long)D->words);
	} else if (r != PW_ERR_NOT_FOUND || nlen > 0) {
		/* a failed load leaves an empty, nameless document */
		if (!D->name[0])
			have_doc = 0;
		say(1, "Could not open: %s", err_text(r));
	}
	return r;
}

static void name_entered(void) {
	uint8_t name[PW_MAX_NAME + 1];
	int n = input_len;
	while (n > 0 && input[n - 1] == ' ')
		n--;
	if (n == 0)
		return;
	memcpy(name, input, (size_t)n);
	if (!memchr(name, '.', (size_t)n)) {
		if (n + 3 > PW_MAX_NAME) {
			say(1, "Name too long");
			return;
		}
		memcpy(name + n, ".md", 3);
		n += 3;
	}
	name[n] = 0;
	int r = doc_create(D, name, n);
	if (r == PW_OK) {
		have_doc = 1;
		reset_view();
		state = S_EDIT;
		say(0, "New document: %s", (const char *)name);
	} else {
		say(1, "Could not create: %s", err_text(r));
	}
}

/* ======================================================================
 * AI chat
 * ====================================================================== */

static void chat_add(const char *s, int n) {
	if (n > CHATLOG_MAX / 2) {               /* keep the end of very long text */
		s += n - CHATLOG_MAX / 2;
		n = CHATLOG_MAX / 2;
	}
	if (chatlog_len + n > CHATLOG_MAX) {     /* drop the oldest part */
		int drop = chatlog_len + n - CHATLOG_MAX;
		const char *nl = memchr(chatlog + drop, '\n', (size_t)(chatlog_len - drop));
		if (nl)
			drop = (int)(nl - chatlog) + 1;
		memmove(chatlog, chatlog + drop, (size_t)(chatlog_len - drop));
		chatlog_len -= drop;
	}
	memcpy(chatlog + chatlog_len, s, (size_t)n);
	chatlog_len += n;
	chatlog[chatlog_len] = 0;
	chat_scroll = 0;
	redraw = 1;
}

static void chat_line(const char *s) {
	if (chatlog_len > 0 && chatlog[chatlog_len - 1] != '\n')
		chat_add("\n", 1);
	chat_add(s, (int)strlen(s));
	chat_add("\n", 1);
}

static void answer_append(const uint8_t *s, int n) {
	if (answer_len + n > ANSWER_MAX)
		n = ANSWER_MAX - answer_len;
	if (n <= 0)
		return;
	if (answer_len + n > answer_cap) {
		int cap = answer_cap ? answer_cap * 2 : 4096;
		while (cap < answer_len + n)
			cap *= 2;
		uint8_t *a = realloc(answer, (size_t)cap);
		if (!a)
			return;
		answer = a;
		answer_cap = cap;
	}
	memcpy(answer + answer_len, s, (size_t)n);
	answer_len += n;
}

static void chat_open(void) {
	state = S_CHAT;
	if (chatlog_len == 0)
		chat_line("-- Ask the AI about your document. It sees the saved text and where"
		          " the cursor is. F4 inserts its last answer at the cursor, F5 (twice)"
		          " replaces the whole document. /new starts a fresh conversation.");
	ensure_visible();
	redraw = 1;
}

static void chat_send(void) {
	if (question_len == 0)
		return;
	if (ai_waiting) {
		say(1, "Still waiting for the answer (Esc cancels)");
		return;
	}
	if (!(flags_now & PW_FLAG_AI)) {
		say(1, "The AI bridge is not running on the PC (pc/ai_bridge.py)");
		return;
	}
	/* the PC reads the document from the card: save it first */
	if (D->modified && do_save(0, 0) != PW_OK)
		return;

	static uint8_t args[8 + PW_AI_MAX_QUESTION];
	pw_put32(args, (uint32_t)cp);
	pw_put32(args + 4, (uint32_t)co);
	memcpy(args + 8, question, (size_t)question_len);
	int r = link_call(PW_OP_AI_ASK, args, 8 + question_len, 0, 0, 0, 30000);
	if (r != PW_OK) {
		say(1, "Could not ask: %s", r == PW_ERR_NOT_AVAILABLE ?
		    "the AI bridge is not running on the PC" : err_text(r));
		return;
	}
	question[question_len] = 0;
	memcpy(last_question, question, (size_t)question_len + 1);
	if (chatlog_len > 0 && chatlog[chatlog_len - 1] != '\n')
		chat_add("\n", 1);
	chat_add("You: ", 5);
	chat_add((const char *)question, question_len);
	chat_add("\n", 1);
	question_len = 0;
	answer_len = 0;
	answer_started = 0;
	ai_waiting = 1;
	ai_last_poll = plat_ms();
}

static void ai_poll(void) {
	static uint8_t buf[PW_MAX_PAYLOAD];
	int n = 0;
	ai_last_poll = plat_ms();
	int r = link_call(PW_OP_AI_POLL, 0, 0, buf, sizeof buf, &n, 3000);
	if (r != PW_OK || n < 1) {
		ai_waiting = 0;
		chat_line("-- The link to the Pico failed while waiting.");
		return;
	}
	const int st = buf[0];
	const uint8_t *text = buf + 1;
	const int tn = n - 1;
	if (st == PW_AI_ERROR) {
		char line[ED_COLS * 3];
		snprintf(line, sizeof line, "-- AI error: %.*s", tn, (const char *)text);
		chat_line(line);
		ai_waiting = 0;
		return;
	}
	if (tn > 0) {
		if (!answer_started) {
			chat_add("AI: ", 4);
			answer_started = 1;
		}
		chat_add((const char *)text, tn);
		answer_append(text, tn);
	}
	if (st == PW_AI_DONE || st == PW_AI_IDLE) {
		if (!answer_started)
			chat_line("AI: (no answer)");
		else
			chat_add("\n", 1);
		ai_waiting = 0;
		say(0, "Answer complete: F4 inserts it at the cursor");
	}
}

static void ai_cancel(void) {
	link_call(PW_OP_AI_CANCEL, 0, 0, 0, 0, 0, 2000);
	ai_waiting = 0;
	chat_line("-- cancelled");
}

static void answer_insert(void) {
	if (!answer_len) {
		say(1, "No answer to insert yet");
		return;
	}
	state = S_EDIT;
	int ok = insert_text(answer, answer_len) == 0;
	edited(ok);
	if (ok)
		say(0, "Answer inserted (%d bytes)", answer_len);
	ensure_visible();
}

static void answer_replace(void) {
	if (!answer_len) {
		say(1, "No answer yet");
		return;
	}
	const uint32_t now = plat_ms();
	if (!replace_armed_until || (int32_t)(now - replace_armed_until) > 0) {
		replace_armed_until = now + 3000;
		say(1, "Press F5 again to REPLACE the whole document with the answer");
		return;
	}
	replace_armed_until = 0;
	doc_clear(D);                            /* keeps the name */
	cp = co = tp = tr = 0;
	state = S_EDIT;
	int ok = insert_text(answer, answer_len) == 0;
	D->modified = 1;
	edited(ok);
	say(0, "Document replaced by the answer - Ctrl+S saves it (backups keep the old one)");
	ensure_visible();
}

/* ======================================================================
 * keys
 * ====================================================================== */

static void input_key(const link_event_t *e) {
	if (e->kind == PW_EV_CHAR) {
		const uint8_t c = e->code;
		if (c < 0x20 || input_len >= PW_MAX_NAME - 3)
			return;
		if (state == S_NAME && (strchr("/\\:*?\"<>|", c) || (input_len == 0 && (c == '.' || c == ' '))))
			return;
		input[input_len++] = c;
		input[input_len] = 0;
	} else if (e->kind == PW_EV_KEY) {
		if (e->code == PW_KEY_BACKSPACE && input_len > 0) {
			input[--input_len] = 0;
		} else if (e->code == PW_KEY_ESC) {
			if (state == S_FIND || have_doc)
				state = S_EDIT;
			else if (back_state == S_OPEN)
				state = S_OPEN;          /* back to the file list */
			/* else: no document and no list - stay here */
		} else if (e->code == PW_KEY_ENTER) {
			if (state == S_NAME) {
				name_entered();
			} else {
				memcpy(find_str, input, (size_t)input_len + 1);
				state = S_EDIT;
				find_next();
			}
		}
	}
	redraw = 1;
}

static void open_key(const link_event_t *e) {
	const int page = text_rows() - 8;
	if (e->kind == PW_EV_CTRL && e->code == 'n') {
		start_name_dialog("");
		return;
	}
	if (e->kind != PW_EV_KEY)
		return;
	switch (e->code) {
	case PW_KEY_UP:   if (fsel > 0) fsel--; break;
	case PW_KEY_DOWN: if (fsel + 1 < nfiles) fsel++; break;
	case PW_KEY_PGUP: fsel = fsel > page ? fsel - page : 0; break;
	case PW_KEY_PGDN: fsel = fsel + page < nfiles ? fsel + page : nfiles - 1; break;
	case PW_KEY_HOME: fsel = 0; break;
	case PW_KEY_END:  fsel = nfiles - 1; break;
	case PW_KEY_ESC:
		if (have_doc) state = S_EDIT;
		break;
	case PW_KEY_ENTER:
		if (nfiles > 0)
			open_file(files[fsel].name, (int)strlen((const char *)files[fsel].name));
		break;
	}
	redraw = 1;
}

/* keys that work everywhere once a document is open: screen size */
static int screen_key(const link_event_t *e) {
	if (e->kind != PW_EV_KEY || (e->code != PW_KEY_F1 + 10 && e->code != PW_KEY_F1 + 11))
		return 0;
	set_rows(ed_rows + (e->code == PW_KEY_F1 + 11 ? 1 : -1));   /* F12 more, F11 fewer */
	say(0, "Screen: %d rows (F11 fewer, F12 more)", ed_rows);
	if (!saving_now && (flags_now & PW_FLAG_SD))
		save_settings();
	if (have_doc)
		ensure_visible();
	return 1;
}

static void chat_key(const link_event_t *e) {
	if (e->kind == PW_EV_CHAR) {
		if (e->code >= 0x20 && question_len < PW_AI_MAX_QUESTION)
			question[question_len++] = e->code;
	} else if (e->kind == PW_EV_KEY) {
		switch (e->code) {
		case PW_KEY_BACKSPACE: if (question_len > 0) question_len--; break;
		case PW_KEY_ENTER:     chat_send(); break;
		case PW_KEY_ESC:
			if (ai_waiting) ai_cancel();
			else state = S_EDIT;
			break;
		case PW_KEY_F1 + 1:    state = S_EDIT; break;            /* F2 */
		case PW_KEY_F1 + 3:    answer_insert(); break;           /* F4 */
		case PW_KEY_F1 + 4:    answer_replace(); break;          /* F5 */
		case PW_KEY_UP:                                           /* recall */
			question_len = (int)strlen((const char *)last_question);
			memcpy(question, last_question, (size_t)question_len);
			break;
		case PW_KEY_PGUP:      chat_scroll += CHAT_LINES - 1; break;
		case PW_KEY_PGDN:
			chat_scroll -= CHAT_LINES - 1;
			if (chat_scroll < 0) chat_scroll = 0;
			break;
		}
	}
	if (state == S_EDIT)
		ensure_visible();
	redraw = 1;
}

static void edit_key(const link_event_t *e) {
	const int ctrl = e->mods & PW_MOD_CTRL;

	if (saving_now && (e->kind == PW_EV_CTRL ||
	                   (e->kind == PW_EV_KEY && e->code >= PW_KEY_F1))) {
		say(1, "Saving - try again when it is done");
		return;
	}

	if (e->kind == PW_EV_CHAR) {
		if (e->code >= 0x20)
			ins_char(e->code);
	} else if (e->kind == PW_EV_CTRL) {
		switch (e->code) {
		case 's': do_save(0, 1); break;
		case 'o': start_open_dialog(); break;
		case 'n': if (leave_doc()) start_name_dialog(""); break;
		case 'f':
			input_len = (int)strlen((const char *)find_str);
			memcpy(input, find_str, (size_t)input_len + 1);
			back_state = S_EDIT;
			state = S_FIND;
			break;
		case 'g': find_next(); break;
		case 'h': state = S_HELP; break;
		}
	} else if (e->kind == PW_EV_KEY) {
		const int vertical = e->code == PW_KEY_UP || e->code == PW_KEY_DOWN ||
		                     e->code == PW_KEY_PGUP || e->code == PW_KEY_PGDN;
		const int page = text_rows() - 1;
		if (!vertical)
			goal = -1;
		switch (e->code) {
		case PW_KEY_LEFT:  ctrl ? mv_word_left() : mv_left(); break;
		case PW_KEY_RIGHT: ctrl ? mv_word_right() : mv_right(); break;
		case PW_KEY_UP:
			if (ctrl) { if (co == 0 && cp > 0) cp--; co = 0; goal = -1; }
			else mv_up();
			break;
		case PW_KEY_DOWN:
			if (ctrl) { if (cp + 1 < D->n) cp++, co = 0; else co = P(cp)->len; goal = -1; }
			else mv_down();
			break;
		case PW_KEY_HOME:
			if (ctrl) cp = 0, co = 0;
			else row_of(cp, co, &co);
			break;
		case PW_KEY_END:
			if (ctrl) { cp = D->n - 1; co = P(cp)->len; }
			else {
				int s;
				row_of(cp, co, &s);
				int nx = row_next(P(cp), s);
				co = nx < 0 ? P(cp)->len : nx - 1;
			}
			break;
		case PW_KEY_PGUP:
			for (int i = 0; i < page; i++) mv_up();
			top_back(page);
			break;
		case PW_KEY_PGDN:
			for (int i = 0; i < page; i++) mv_down();
			top_fwd(page);
			break;
		case PW_KEY_BACKSPACE: key_backspace(ctrl); break;
		case PW_KEY_DELETE:    key_delete(); break;
		case PW_KEY_ENTER:     key_enter(); break;
		case PW_KEY_TAB:
			for (int i = 0; i < TAB_SPACES; i++) ins_char(' ');
			break;
		case PW_KEY_INSERT:    ed_overwrite = !ed_overwrite; break;
		case PW_KEY_ESC:       msg_until = 0; break;
		case PW_KEY_F1:        state = S_HELP; break;
		case PW_KEY_F1 + 1:    chat_open(); break;        /* F2 */
		case PW_KEY_F1 + 2:    find_next(); break;        /* F3 */
		case PW_KEY_F1 + 3:    answer_insert(); break;    /* F4 */
		}
	}
	ensure_visible();
	redraw = 1;
}

void ed_key(const link_event_t *e) {
	if (state != S_CONNECT && screen_key(e))
		return;
	switch (state) {
	case S_EDIT: edit_key(e); break;
	case S_CHAT: chat_key(e); break;
	case S_OPEN: open_key(e); break;
	case S_NAME:
	case S_FIND: input_key(e); break;
	case S_HELP: state = have_doc ? S_EDIT : S_CONNECT; redraw = 1; break;
	case S_CONNECT:
		if (e->kind == PW_EV_KEY && e->code == PW_KEY_F1) { state = S_HELP; redraw = 1; }
		break;
	}
}

/* ======================================================================
 * start-up and timers
 * ====================================================================== */

static void try_connect(void) {
	uint8_t buf[16];
	int n = 0;
	int r = link_call(PW_OP_HELLO, 0, 0, buf, sizeof buf, &n, 500);
	if (r != PW_OK || n < 2) {
		say(1, "No Pico found - check the cable and controller port 2");
		return;
	}
	if (buf[0] != PW_PROTOCOL_VERSION) {
		say(1, "The Pico firmware does not match this disc (protocol %d)", buf[0]);
		return;
	}
	if (!(buf[1] & PW_FLAG_SD)) {
		say(1, "Waiting for the SD card...");
		return;
	}
	load_settings();
	/* reopen the last document, or let the user pick / create one */
	r = open_file((const uint8_t *)"", 0);
	if (r != PW_OK && r != LINK_ERR_TIMEOUT)
		start_open_dialog();         /* no last document (or it failed) */
}

void ed_init(void (*present)(void)) {
	present_fn = present;
	doc_init(D);
	state = S_CONNECT;
	have_doc = 0;
	chatlog_len = 0;
	chatlog[0] = 0;
	question_len = 0;
	answer_len = 0;
	ai_waiting = 0;
	next_connect = plat_ms() + 300;
	say(0, "Connecting to the Pico...");
}

void ed_tick(uint8_t flags) {
	const uint32_t now = plat_ms();

	if (flags != flags_now) {
		flags_now = flags;
		redraw = 1;
	}
	if (msg_until && (int32_t)(now - msg_until) >= 0) {
		msg_until = 0;
		redraw = 1;
	}

	if (state == S_CONNECT && (int32_t)(now - next_connect) >= 0) {
		try_connect();
		next_connect = plat_ms() + 1000;
	}

	if (ai_waiting && !saving_now && now - ai_last_poll >= AI_POLL_MS)
		ai_poll();

	if ((state == S_EDIT || state == S_CHAT) && have_doc && D->modified && !saving_now &&
	    !ai_waiting && now - last_edit >= AUTOSAVE_IDLE_MS && now - last_save >= AUTOSAVE_EVERY_MS)
		do_save(1, 1);
}

void ed_frame(void) {
	link_event_t ev[PW_MAX_EVENTS];
	uint8_t flags = 0;
	const int n = link_keys(ev, PW_MAX_EVENTS, &flags);
	if (n < 0)
		flags = ED_NO_PICO;
	for (int i = 0; i < n; i++)
		ed_key(&ev[i]);
	ed_tick(flags);
	ed_render();
	last_frame = plat_ms();
	if (present_fn)
		present_fn();
}

/* ======================================================================
 * drawing
 * ====================================================================== */

static void put(int y, int x, const char *s, int attr, int max_x) {
	for (; *s && x < max_x && x < ED_COLS; s++, x++) {
		ed_screen[y][x].ch = (uint8_t)*s;
		ed_screen[y][x].attr = (uint8_t)attr;
	}
}

static void putn(int y, int x, const uint8_t *s, int n, int attr) {
	for (int i = 0; i < n && x + i < ED_COLS; i++) {
		uint8_t c = s[i];
		ed_screen[y][x + i].ch = (c == '\t') ? ' ' : (c < 0x20 ? '?' : c);
		ed_screen[y][x + i].attr = (uint8_t)attr;
	}
}

static void fill(int y, int x0, int x1, int attr) {
	for (int x = x0; x < x1 && x < ED_COLS; x++) {
		ed_screen[y][x].ch = ' ';
		ed_screen[y][x].attr = (uint8_t)attr;
	}
}

static void draw_text(void) {
	const int rows = text_rows();
	int pi = tp, s = row_start(tp, tr);
	for (int y = 0; y < rows && pi < D->n; y++) {
		const para_t *p = P(pi);
		const int nx = row_next(p, s);
		const int e = nx < 0 ? p->len : nx;
		const int attr = (p->len > 0 && p->t[0] == '#') ? A_HEAD : A_TEXT;
		putn(y, 0, p->t + s, e - s, attr);
		if (pi == cp && co >= s && (nx < 0 || co < nx)) {
			ed_cur_y = y;
			ed_cur_x = co - s < ED_COLS ? co - s : ED_COLS - 1;
		}
		if (nx < 0) pi++, s = 0;
		else s = nx;
	}
}

static void draw_status(void) {
	const int y = ed_rows - 1;
	char left[PW_MAX_NAME + 8], warn[24], info[48];
	fill(y, 0, ED_COLS, A_STATUS);

	snprintf(left, sizeof left, " %s%s ", have_doc ? (const char *)D->name : "ps1-writer",
	         (have_doc && D->modified) ? "*" : "");
	put(y, 0, left, A_STATUS, ED_COLS);
	const int x = (int)strlen(left);

	/* right end: warnings, then word count and mode */
	if (flags_now & ED_NO_PICO)
		snprintf(warn, sizeof warn, " NO PICO ");
	else if (!(flags_now & PW_FLAG_KEYBOARD))
		snprintf(warn, sizeof warn, " NO KEYBOARD ");
	else if (!(flags_now & PW_FLAG_SD))
		snprintf(warn, sizeof warn, " NO SD CARD ");
	else
		warn[0] = 0;
	int right = ED_COLS - (int)strlen(warn);
	put(y, right, warn, A_WARN, ED_COLS);
	if (have_doc) {
		snprintf(info, sizeof info, " %s%lu words %s ",
		         (flags_now & PW_FLAG_AI) && !(flags_now & ED_NO_PICO) ? "AI  " : "",
		         (unsigned long)D->words, ed_overwrite ? "OVR" : "INS");
		const int start = right - (int)strlen(info);
		put(y, start, info, A_STATUS, right);
		right = start;
	}

	/* middle: a message, or the cursor position */
	if (msg_until) {
		put(y, x, "| ", A_STATUS, right);
		put(y, x + 2, msg, msg_warn ? A_WARN : A_STATUS, right);
	} else if (have_doc && (state == S_EDIT || state == S_CHAT)) {
		snprintf(info, sizeof info, "| Par %d/%d  Col %d", cp + 1, D->n, co + 1);
		put(y, x, info, A_STATUS, right);
	}
}

/* the chat panel above the status bar */
static void draw_chat(void) {
	const int y0 = ed_rows - 1 - CHAT_H;

	fill(y0, 0, ED_COLS, A_DLG_TITLE);
	put(y0, 1, ai_waiting ? "AI chat - waiting for the answer...  (Esc cancels)"
	                      : "AI chat   Enter ask   F4 insert   F5 replace doc   PgUp/PgDn   F2 close",
	    A_DLG_TITLE, ED_COLS);

	/* wrap the log into lines; show the last CHAT_LINES (minus scroll) */
	static struct { int start, len, attr; } lines[CHATLOG_MAX / 8];
	int nl = 0;
	const uint8_t *t = (const uint8_t *)chatlog;
	for (int ps = 0; ps < chatlog_len && nl < (int)(sizeof lines / sizeof lines[0]);) {
		const uint8_t *e = memchr(t + ps, '\n', (size_t)(chatlog_len - ps));
		const int pl = e ? (int)(e - (t + ps)) : chatlog_len - ps;
		const int attr = (pl >= 4 && memcmp(t + ps, "You:", 4) == 0) ? A_DLG_TITLE : A_DLG;
		int s = 0;
		do {
			int nx = wrap_next(t + ps, pl, s, ED_COLS);
			int end = nx < 0 ? pl : nx;
			if (nl < (int)(sizeof lines / sizeof lines[0])) {
				lines[nl].start = ps + s;
				lines[nl].len = end - s;
				lines[nl].attr = attr;
				nl++;
			}
			if (nx < 0) break;
			s = nx;
		} while (1);
		ps += pl + 1;
	}
	if (chat_scroll > nl - CHAT_LINES)
		chat_scroll = nl - CHAT_LINES > 0 ? nl - CHAT_LINES : 0;
	int first = nl - CHAT_LINES - chat_scroll;
	if (first < 0) first = 0;
	for (int i = 0; i < CHAT_LINES; i++) {
		const int y = y0 + 1 + i;
		fill(y, 0, ED_COLS, A_DLG);
		if (first + i < nl)
			putn(y, 0, t + lines[first + i].start, lines[first + i].len, lines[first + i].attr);
	}

	/* input line: the end of the question if it is long */
	const int yi = y0 + CHAT_H - 1;
	fill(yi, 0, ED_COLS, A_DLG_SEL);
	put(yi, 0, "> ", A_DLG_SEL, ED_COLS);
	const int room = ED_COLS - 3;
	const int from = question_len > room ? question_len - room : 0;
	putn(yi, 2, question + from, question_len - from, A_DLG_SEL);
	ed_cur_y = yi;
	ed_cur_x = 2 + question_len - from;
}

/* a framed box; returns the inner left column */
static int box(int y0, int h, int w, const char *title) {
	const int x0 = (ED_COLS - w) / 2;
	for (int y = y0; y < y0 + h; y++)
		fill(y, x0, x0 + w, A_DLG);
	for (int x = x0; x < x0 + w; x++) {
		ed_screen[y0][x].ch = '-';
		ed_screen[y0 + h - 1][x].ch = '-';
	}
	for (int y = y0; y < y0 + h; y++) {
		ed_screen[y][x0].ch = '|';
		ed_screen[y][x0 + w - 1].ch = '|';
	}
	ed_screen[y0][x0].ch = ed_screen[y0][x0 + w - 1].ch = '+';
	ed_screen[y0 + h - 1][x0].ch = ed_screen[y0 + h - 1][x0 + w - 1].ch = '+';
	char t[64];
	snprintf(t, sizeof t, " %s ", title);
	put(y0, x0 + 2, t, A_DLG_TITLE, x0 + w - 2);
	return x0 + 2;
}

static void draw_open(void) {
	const int w = 64, h = ed_rows - 1 - 4, list_h = h - 4, y0 = 2;
	const int x = box(y0, h, w, "Open (Enter)   New: Ctrl+N   Cancel: Esc");
	if (fsel < ftop) ftop = fsel;
	if (fsel >= ftop + list_h) ftop = fsel - list_h + 1;
	for (int i = 0; i < list_h && ftop + i < nfiles; i++) {
		const int f = ftop + i, y = y0 + 2 + i;
		const int attr = (f == fsel) ? A_DLG_SEL : A_DLG;
		char line[80];
		snprintf(line, sizeof line, " %-46s %7lu KB ", (const char *)files[f].name,
		         (unsigned long)((files[f].size + 1023) / 1024));
		fill(y, x, x + w - 4, attr);
		put(y, x, line, attr, x + w - 4);
	}
	char foot[48];
	snprintf(foot, sizeof foot, "%d document%s", nfiles, nfiles == 1 ? "" : "s");
	put(y0 + h - 2, x, foot, A_DLG, x + w - 4);
}

static void draw_input(void) {
	const int w = 60, y0 = (ed_rows - 6) / 2;
	const int x = box(y0, 5, w, state == S_NAME ? "New document name (Enter / Esc)"
	                                            : "Find (Enter / Esc)");
	fill(y0 + 2, x, x + w - 4, A_DLG_SEL);
	put(y0 + 2, x, (const char *)input, A_DLG_SEL, x + w - 4);
	if (state == S_NAME)
		put(y0 + 3, x, ".md is added if there is no extension", A_DLG, x + w - 4);
	ed_cur_y = y0 + 2;
	ed_cur_x = x + input_len;
}

static const char *const help[] = {
	"Ctrl+S        Save (also automatic: after a pause, once a minute)",
	"Ctrl+O        Open a document      Ctrl+N    New document",
	"Ctrl+F        Find                 F3 / Ctrl+G   Find next",
	"Arrows        Move                 Ctrl+Left/Right   Word by word",
	"Ctrl+Up/Down  Start of paragraph / next paragraph",
	"Home / End    Start / end of line  (with Ctrl: of the document)",
	"PgUp / PgDn   Page                 Insert    Insert / overwrite",
	"Ctrl+Backspace  Delete word left   Tab       4 spaces",
	"F2            AI chat (needs pc/ai_bridge.py running on the PC)",
	"F4            Insert the last AI answer at the cursor",
	"F11 / F12     Fewer / more screen rows (if lines are cut off)",
	"",
	"Documents are in /ps1writer on the SD card (UTF-8, open on any PC).",
	"Each save moves the old version to /ps1writer/backup (5 kept).",
	"Lines starting with # are shown as headings (Markdown).",
	"",
	"Press any key to close.",
};

static void draw_help(void) {
	const int n = (int)(sizeof help / sizeof help[0]);
	int y0 = (ed_rows - 1 - (n + 4)) / 2;
	if (y0 < 0) y0 = 0;
	const int x = box(y0, n + 4, 74, "ps1-writer - Help (F1)");
	for (int i = 0; i < n && y0 + 2 + i < ed_rows - 1; i++)
		put(y0 + 2 + i, x, help[i], A_DLG, x + 70);
}

static void draw_splash(void) {
	const int y = (ed_rows - 1) / 2 - 3;
	put(y, (ED_COLS - 10) / 2, "ps1-writer", A_HEAD, ED_COLS);
	put(y + 2, (ED_COLS - 32) / 2, "A text editor for the PlayStation", A_TEXT, ED_COLS);
	put(y + 4, (ED_COLS - 16) / 2, "F1: help and keys", A_TEXT, ED_COLS);
}

int ed_render(void) {
	if (!redraw)
		return 0;
	redraw = 0;
	for (int y = 0; y < ED_MAX_ROWS; y++)
		fill(y, 0, ED_COLS, A_TEXT);
	ed_cur_x = ed_cur_y = -1;

	if (have_doc)
		draw_text();
	else
		draw_splash();
	if (state != S_EDIT)
		ed_cur_x = ed_cur_y = -1;
	draw_status();

	switch (state) {
	case S_CHAT: draw_chat(); break;
	case S_OPEN: draw_open(); break;
	case S_NAME:
	case S_FIND: draw_input(); break;
	case S_HELP: draw_help(); break;
	}
	return 1;
}
