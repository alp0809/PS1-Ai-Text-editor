/*
 * ai_link_test.c - the Pico's AI link code (pico/ai.c + the file server)
 * talking to the real pc/ai_bridge.py through a pseudo-terminal.
 * Started by test_bridge.py, which runs the bridge on the other end.
 *
 *   ai_link_test <fd> <sd folder>      exit 0 = the answer came back right
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include "ai.h"
#include "fileserver.h"
#include "pw_protocol.h"
#include "shared.h"

void vfs_posix_root(const char *dir);
static int fd;

int aiport_connected(void) { return 1; }

int aiport_write(const void *p, uint32_t n) {
	const uint8_t *b = p;
	while (n) {
		ssize_t k = write(fd, b, n);
		if (k <= 0) return -1;
		b += k;
		n -= (uint32_t)k;
	}
	return 0;
}

int aiport_read(uint8_t *buf, int max) {
	struct pollfd pf = { fd, POLLIN, 0 };
	if (poll(&pf, 1, 0) <= 0) return 0;
	ssize_t k = read(fd, buf, (size_t)max);
	return k > 0 ? (int)k : 0;
}

static uint16_t call(uint8_t op, const uint8_t *args, int alen, uint8_t *resp) {
	uint8_t req[PW_MAX_PAYLOAD];
	req[0] = op;
	req[1] = 1;
	memcpy(req + 2, args, (size_t)alen);
	return fileserver_process(req, (uint16_t)(alen + 2), resp);
}

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* ask and collect the answer; returns it (Latin-1) or NULL */
static char *ask(const char *q) {
	static char answer[70000];
	uint8_t args[8 + PW_AI_MAX_QUESTION], resp[PW_MAX_PAYLOAD];
	int alen = 8 + (int)strlen(q);
	pw_put32(args, 1);                  /* cursor: paragraph 1, offset 4 */
	pw_put32(args + 4, 4);
	memcpy(args + 8, q, strlen(q));
	if (call(PW_OP_AI_ASK, args, alen, resp) < 3 || resp[2] != PW_OK) {
		printf("ASK failed: %d\n", resp[2]);
		return NULL;
	}
	int len = 0;
	double give_up = now() + 20;
	while (now() < give_up) {
		ai_poll();
		uint16_t n = call(PW_OP_AI_POLL, 0, 0, resp);
		int st = resp[3];
		memcpy(answer + len, resp + 4, (size_t)(n - 4));
		len += n - 4;
		answer[len] = 0;
		if (st == PW_AI_ERROR) { printf("AI error: %s\n", answer); return NULL; }
		if (st == PW_AI_DONE) return answer;
		usleep(5000);
	}
	printf("timeout; got so far: %s\n", answer);
	return NULL;
}

int main(int argc, char **argv) {
	if (argc < 3) return 2;
	fd = atoi(argv[1]);
	vfs_posix_root(argv[2]);
	shared_reset();
	fileserver_init();
	ai_init();

	/* open the test document */
	uint8_t resp[PW_MAX_PAYLOAD];
	const char *name = "doc.md";
	if (call(PW_OP_OPEN, (const uint8_t *)name, (int)strlen(name), resp) < 3 || resp[2] != PW_OK) {
		printf("open failed\n");
		return 1;
	}
	/* wait for the bridge's HELLO */
	double give_up = now() + 10;
	while (!ai_ready() && now() < give_up) {
		ai_poll();
		usleep(10000);
	}
	if (!ai_ready()) { printf("no HELLO from the bridge\n"); return 1; }

	char *a = ask("Caf\xE9 question?");
	if (!a) return 1;
	printf("ANSWER1: %s\n", a);
	if (!strstr(a, "You asked: Caf\xE9 question?") || !strstr(a, "Text before the cursor: ...") ||
	    !strstr(a, "Seco")) {
		printf("unexpected answer\n");
		return 1;
	}
	a = ask("/new");
	if (!a || !strstr(a, "new conversation")) return 1;
	printf("ANSWER2: %s\n", a);
	printf("OK\n");
	return 0;
}
