#include <string.h>
#include "shared.h"

pw_event_t        keyq[KEYQ_SIZE];
volatile uint32_t keyq_head;
volatile uint32_t keyq_tail;

volatile uint8_t  pw_flags;

volatile int      mb_state;
volatile int      mb_resp_valid;
uint8_t           mb_req[PW_MAX_PAYLOAD];
volatile uint16_t mb_req_len;
uint8_t           mb_resp[PW_MAX_PAYLOAD];
volatile uint16_t mb_resp_len;

int keyq_push(uint8_t kind, uint8_t code, uint8_t mods) {
	uint32_t h = keyq_head;
	if (h - keyq_tail >= KEYQ_SIZE)
		return 0;
	pw_event_t *e = &keyq[h & (KEYQ_SIZE - 1)];
	e->kind = kind;
	e->code = code;
	e->mods = mods;
	MEM_BARRIER();           /* event visible before the new head */
	keyq_head = h + 1;
	return 1;
}

void shared_reset(void) {
	keyq_head = keyq_tail = 0;
	pw_flags = 0;
	mb_state = MB_IDLE;
	mb_resp_valid = 0;
	mb_req_len = mb_resp_len = 0;
}
