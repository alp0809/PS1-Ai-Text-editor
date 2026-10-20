/*
 * bus_logic.c - the Pico's side of the PS1 protocol (see pw_protocol.h).
 *
 * Runs on core 0 between bytes, so it must stay fast and must not call
 * anything slow (no flash access, no printing, no SD card).
 * Every byte of our own transactions is ACKed; the PS1 simply stops
 * clocking (and deselects) when it has what it needs.
 */

#include "bus_logic.h"

#ifdef PICO_BUILD
#include "pico/platform.h"
#define RAM_FUNC(f) __not_in_flash_func(f)
#else
#define RAM_FUNC(f) f
#endif

static inline uint16_t crc16_byte(uint16_t crc, uint8_t v) {
	crc ^= (uint16_t)v << 8;
	for (int i = 0; i < 8; i++)
		crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	return crc;
}

uint8_t RAM_FUNC(bus_begin)(bus_t *b) {
	b->ex = 0;
	b->ours = 1;
	b->cmd = 0;
	b->tx_len = 0;
	b->keys_n = 0;
	b->accepting = 0;
	b->resp_total = 0;
	return 0xFF;                       /* reply for the address byte */
}

void RAM_FUNC(bus_end)(bus_t *b) {
	b->ours = 0;
}

/* the response byte at position i (0 = status) */
static inline uint8_t RAM_FUNC(resp_byte)(bus_t *b, int i) {
	const int len = mb_resp_len;
	if (i == 0) return b->resp_status;
	if (i == 1) return (uint8_t)len;
	if (i == 2) return (uint8_t)(len >> 8);
	if (i < 3 + len) return mb_resp[i - 3];
	if (i == 3 + len) return (uint8_t)b->resp_crc;
	if (i == 4 + len) return (uint8_t)(b->resp_crc >> 8);
	return 0xFF;
}

static void RAM_FUNC(start_keys)(bus_t *b) {
	uint32_t tail = keyq_tail;
	uint32_t avail = keyq_head - tail;
	MEM_BARRIER();                     /* read events after seeing the head */
	int n = avail > PW_MAX_EVENTS ? PW_MAX_EVENTS : (int)avail;

	b->tx[0] = pw_flags;
	b->tx[1] = (uint8_t)n;
	for (int i = 0; i < n; i++) {
		const pw_event_t *e = &keyq[(tail + i) & (KEYQ_SIZE - 1)];
		b->tx[2 + 3 * i] = e->kind;
		b->tx[3 + 3 * i] = e->code;
		b->tx[4 + 3 * i] = e->mods;
	}
	b->keys_n = n;
	b->tx_len = 2 + 3 * n;
}

static void RAM_FUNC(start_response)(bus_t *b) {
	if (mb_state != MB_IDLE) {
		b->resp_status = PW_ST_BUSY;
		b->resp_total = 1;
	} else if (mb_resp_valid) {
		const int len = mb_resp_len;
		uint16_t crc = PW_CRC16_INIT;
		crc = crc16_byte(crc, (uint8_t)len);
		crc = crc16_byte(crc, (uint8_t)(len >> 8));
		for (int i = 0; i < len; i++)
			crc = crc16_byte(crc, mb_resp[i]);
		b->resp_crc = crc;
		b->resp_status = PW_ST_READY;
		b->resp_total = 5 + len;
	} else {
		b->resp_status = PW_ST_NONE;
		b->resp_total = 1;
	}
}

int RAM_FUNC(bus_byte)(bus_t *b, uint8_t rx, uint8_t *next) {
	const int ex = b->ex++;
	*next = 0xFF;

	if (!b->ours)
		return 0;

	if (ex == 0) {                     /* address */
		if (rx != PW_DEV_ADDR) {
			b->ours = 0;               /* someone else's device */
			return 0;
		}
		return 1;
	}

	if (ex == 1) {                     /* command */
		b->cmd = rx;
		switch (rx) {
		case PW_CMD_KEYS:
			start_keys(b);
			*next = b->tx[0];
			return 1;
		case PW_CMD_REQUEST:
			b->accepting = (mb_state == MB_IDLE);
			b->crc = PW_CRC16_INIT;
			*next = b->accepting ? PW_ST_READY : PW_ST_BUSY;
			return 1;
		case PW_CMD_RESPONSE:
			start_response(b);
			*next = b->resp_status;
			return 1;
		default:
			b->ours = 0;
			return 0;
		}
	}

	const int k = ex - 2;              /* data byte index */

	switch (b->cmd) {
	case PW_CMD_KEYS:
		/* tx[k] was sent while rx arrived */
		if (k == b->tx_len - 1 && b->keys_n > 0) {
			MEM_BARRIER();
			keyq_tail += (uint32_t)b->keys_n;   /* PS1 has them all */
			b->keys_n = 0;
		}
		if (k + 1 < b->tx_len)
			*next = b->tx[k + 1];
		return 1;

	case PW_CMD_REQUEST:
		if (!b->accepting) {
			b->ours = 0;
			return k == 0;             /* ACK the status byte, then stop */
		}
		if (k == 0) {
			b->req_len = rx;
			b->crc = crc16_byte(b->crc, rx);
			return 1;
		}
		if (k == 1) {
			b->req_len |= (uint16_t)rx << 8;
			b->crc = crc16_byte(b->crc, rx);
			if (b->req_len > PW_MAX_PAYLOAD) {
				b->accepting = 0;
				b->ours = 0;
				return 0;
			}
			return 1;
		}
		if (k < 2 + b->req_len) {
			mb_req[k - 2] = rx;
			b->crc = crc16_byte(b->crc, rx);
			return 1;
		}
		if (k == 2 + b->req_len) {
			b->crc_lo = rx;
			return 1;
		}
		if (k == 3 + b->req_len) {
			uint16_t got = (uint16_t)(b->crc_lo | (rx << 8));
			if (got == b->crc) {
				mb_req_len = b->req_len;
				mb_resp_valid = 0;
				MEM_BARRIER();         /* request data before the state */
				mb_state = MB_PENDING;
			}
			b->accepting = 0;
			b->ours = 0;
			return 1;
		}
		return 0;

	case PW_CMD_RESPONSE:
		if (k + 1 < b->resp_total)
			*next = resp_byte(b, k + 1);
		else
			b->ours = 0;
		return 1;
	}
	return 0;
}
