/*
 * aiport_fake.c - a pretend PC on the AI link, for the PC tests: it answers
 * every question with "Answer to: <question>" plus a second line, and keeps
 * the document it was sent so the tests can check it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ai.h"

int  fake_pc_on;                     /* the bridge is "running" */
char fake_pc_doc[400000];            /* the last document received (UTF-8) */
long fake_pc_doc_len;
char fake_pc_question[2100];
unsigned fake_pc_para, fake_pc_off;
const char *fake_pc_extra = "";      /* appended to every answer (UTF-8) */
const char *fake_pc_answer;          /* if set: the whole answer instead */

static char inbox[420000];           /* Pico -> PC */
static long inbox_len;
static char outbox[100000];          /* PC -> Pico */
static long out_len, out_pos;
static int  said_hello;

static void out(const void *p, long n) {
	if (out_len + n > (long)sizeof outbox) return;
	memcpy(outbox + out_len, p, (size_t)n);
	out_len += n;
}

static void out_text(const char *kind, const char *s) {
	char h[32];
	snprintf(h, sizeof h, "%s %ld\n", kind, (long)strlen(s));
	out(h, (long)strlen(h));
	out(s, (long)strlen(s));
}

/* complete ASK messages in the inbox get an answer */
static void pc_think(void) {
	for (;;) {
		char *nl = memchr(inbox, '\n', (size_t)inbox_len);
		if (!nl) return;
		unsigned p, o; long dl, ql;
		if (sscanf(inbox, "ASK %u %u %ld %ld", &p, &o, &dl, &ql) == 4) {
			long hl = nl - inbox + 1;
			if (inbox_len < hl + dl + ql) return;           /* not all there yet */
			fake_pc_para = p;
			fake_pc_off = o;
			fake_pc_doc_len = dl;
			memcpy(fake_pc_doc, inbox + hl, (size_t)dl);
			memcpy(fake_pc_question, inbox + hl + dl, (size_t)ql);
			fake_pc_question[ql] = 0;
			char answer[2400];
			if (fake_pc_answer)
				snprintf(answer, sizeof answer, "%s", fake_pc_answer);
			else
				snprintf(answer, sizeof answer, "Answer to: %s\nSecond line.%s",
				         fake_pc_question, fake_pc_extra);
			/* in two pieces, like a streaming model */
			long half = (long)strlen(answer) / 2;
			char a1[1300];
			memcpy(a1, answer, (size_t)half);
			a1[half] = 0;
			out_text("TXT", a1);
			out_text("TXT", answer + half);
			out("END\n", 4);
			long used = hl + dl + ql;
			memmove(inbox, inbox + used, (size_t)(inbox_len - used));
			inbox_len -= used;
		} else {                                            /* CANCEL etc. */
			long used = nl - inbox + 1;
			memmove(inbox, inbox + used, (size_t)(inbox_len - used));
			inbox_len -= used;
		}
	}
}

int aiport_connected(void) {
	if (!fake_pc_on)
		said_hello = 0;              /* says HELLO again when it comes back */
	return fake_pc_on;
}

int aiport_write(const void *p, uint32_t n) {
	if (!fake_pc_on) return -1;
	if (inbox_len + (long)n > (long)sizeof inbox) return -1;
	memcpy(inbox + inbox_len, p, n);
	inbox_len += n;
	pc_think();
	return 0;
}

int aiport_read(uint8_t *buf, int max) {
	if (!fake_pc_on) { said_hello = 0; return 0; }
	if (!said_hello) {
		out("HELLO fake\n", 11);
		said_hello = 1;
	}
	long n = out_len - out_pos;
	if (n > max) n = max;
	memcpy(buf, outbox + out_pos, (size_t)n);
	out_pos += n;
	if (out_pos == out_len) out_pos = out_len = 0;
	return (int)n;
}
