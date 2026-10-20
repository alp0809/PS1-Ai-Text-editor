/*
 * bus_logic.h - what the Pico answers on the controller bus, byte by byte.
 *
 * Pure C with no hardware access, so the same code runs on the Pico
 * (driven by the PIO state machine) and in the PC tests (driven directly
 * by the PS1-side code).
 *
 * Usage for one transaction (ATT low ... ATT high):
 *   reply = bus_begin(&b);                  reply for the first byte
 *   for each byte received:
 *       ack = bus_byte(&b, rx, &reply);     reply for the NEXT byte
 *   bus_end(&b);                            ATT went high
 */

#ifndef BUS_LOGIC_H
#define BUS_LOGIC_H

#include <stdint.h>
#include "shared.h"

typedef struct {
	int      ex;            /* index of the next byte in this transaction */
	int      ours;          /* still talking to us?                        */
	uint8_t  cmd;

	/* KEYS */
	uint8_t  tx[2 + 3 * PW_MAX_EVENTS];
	int      tx_len;
	int      keys_n;

	/* REQUEST */
	int      accepting;
	uint16_t req_len;
	uint16_t crc;
	uint8_t  crc_lo;

	/* RESPONSE */
	int      resp_total;    /* bytes after the command byte */
	uint8_t  resp_status;
	uint16_t resp_crc;
} bus_t;

uint8_t bus_begin(bus_t *b);
int     bus_byte(bus_t *b, uint8_t rx, uint8_t *next_reply);
void    bus_end(bus_t *b);

#endif
