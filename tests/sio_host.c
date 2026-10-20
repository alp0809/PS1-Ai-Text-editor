/*
 * sio_host.c - the PS1's bus driver for the PC tests: every byte goes
 * straight into the Pico's bus logic, and "waiting" runs the Pico's file
 * server. Optional fault injection flips bits in requests and responses.
 */

#include <stdlib.h>
#include "sio.h"
#include "link.h"
#include "bus_logic.h"
#include "fileserver.h"
#include "shared.h"
#include "ai.h"

static bus_t   b;
static uint8_t reply;
static int     in_xfer, byte_no;
static uint32_t now;

int sio_fault_every;          /* 0 = off, else about 1 in N bytes corrupted */
unsigned long sio_faults;

void sio_init(void) {}

void sio_select(void) {
	reply = bus_begin(&b);
	byte_no = 0;
	in_xfer = 1;
}

void sio_deselect(void) {
	bus_end(&b);
	in_xfer = 0;
}

int sio_xfer(uint8_t tx, uint8_t *rx) {
	/* only file transfers are protected by a CRC; keys are not */
	const int faultable = sio_fault_every && byte_no >= 2 &&
	                      (b.cmd == PW_CMD_REQUEST || b.cmd == PW_CMD_RESPONSE);
	if (faultable && rand() % sio_fault_every == 0) {
		tx ^= (uint8_t)(1u << (rand() % 8));
		sio_faults++;
	}
	*rx = reply;
	if (faultable && rand() % sio_fault_every == 0) {
		*rx ^= (uint8_t)(1u << (rand() % 8));
		sio_faults++;
	}
	byte_no++;
	return bus_byte(&b, tx, &reply);
}

/* a slow Pico: requests take this long (to test typing during a save) */
uint32_t host_pico_delay_ms;
void (*host_idle_hook)(void);
static uint32_t pending_since;

uint32_t plat_ms(void) { return now; }

/* what the Pico's core 1 does besides file requests */
void host_pico_background(void) {
	ai_poll();
	if (ai_ready())
		pw_flags |= PW_FLAG_AI;
	else
		pw_flags &= (uint8_t)~PW_FLAG_AI;
}

void plat_idle(void) {
	now += 1;
	if (host_idle_hook)
		host_idle_hook();
	host_pico_background();
	if (mb_state != MB_PENDING)
		return;
	if (!pending_since)
		pending_since = now;
	if (now - pending_since >= host_pico_delay_ms) {
		fileserver_poll();
		pending_since = 0;
	}
}

void host_advance(uint32_t ms) { now += ms; }
