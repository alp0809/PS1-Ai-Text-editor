/* link.c - PS1 side of the ps1-writer protocol (see pw_protocol.h) */

#include <string.h>
#include "link.h"
#include "sio.h"

uint32_t link_retries, link_bus_errors;
void (*link_wait_hook)(void);

static uint8_t seq;
static uint8_t req[PW_MAX_PAYLOAD];
static uint8_t resp[PW_MAX_PAYLOAD];

int link_keys(link_event_t *ev, int max, uint8_t *flags) {
	uint8_t rx, n;
	int i;

	sio_select();
	if (!sio_xfer(PW_DEV_ADDR, &rx) || !sio_xfer(PW_CMD_KEYS, &rx) ||
	    !sio_xfer(0, flags) || !sio_xfer(0, &n)) {
		sio_deselect();
		link_bus_errors++;
		return -1;
	}
	if (n > PW_MAX_EVENTS)
		n = PW_MAX_EVENTS;
	for (i = 0; i < n; i++) {
		link_event_t e;
		if (!sio_xfer(0, &e.kind) || !sio_xfer(0, &e.code) || !sio_xfer(0, &e.mods)) {
			sio_deselect();
			link_bus_errors++;
			return -1;           /* the Pico keeps them; they come again */
		}
		if (i < max)
			ev[i] = e;
	}
	sio_deselect();
	return n < max ? n : max;
}

/* 1 = sent, 0 = Pico busy, -1 = bus error */
static int send_request(const uint8_t *p, int len) {
	uint8_t rx, hdr[2];
	uint16_t crc;
	int i;

	hdr[0] = (uint8_t)len;
	hdr[1] = (uint8_t)(len >> 8);
	crc = pw_crc16(PW_CRC16_INIT, hdr, 2);
	crc = pw_crc16(crc, p, (size_t)len);

	sio_select();
	if (!sio_xfer(PW_DEV_ADDR, &rx) || !sio_xfer(PW_CMD_REQUEST, &rx))
		goto fail;
	if (!sio_xfer(hdr[0], &rx))
		goto fail;
	if (rx != PW_ST_READY) {
		sio_deselect();
		return 0;
	}
	if (!sio_xfer(hdr[1], &rx))
		goto fail;
	for (i = 0; i < len; i++)
		if (!sio_xfer(p[i], &rx))
			goto fail;
	if (!sio_xfer((uint8_t)crc, &rx) || !sio_xfer((uint8_t)(crc >> 8), &rx))
		goto fail;
	sio_deselect();
	return 1;
fail:
	sio_deselect();
	link_bus_errors++;
	return -1;
}

/* PW_ST_READY with *len set, PW_ST_BUSY, PW_ST_NONE, -1 bus error, -2 bad CRC */
static void wait_a_bit(void) {
	plat_idle();
	if (link_wait_hook)
		link_wait_hook();
}

static int read_response(int *len) {
	uint8_t rx, st, lo, hi;
	uint16_t crc;
	int n, i;

	sio_select();
	if (!sio_xfer(PW_DEV_ADDR, &rx) || !sio_xfer(PW_CMD_RESPONSE, &rx) || !sio_xfer(0, &st))
		goto fail;
	if (st != PW_ST_READY) {
		sio_deselect();
		return (st == PW_ST_BUSY || st == PW_ST_NONE) ? st : -1;
	}
	if (!sio_xfer(0, &lo) || !sio_xfer(0, &hi))
		goto fail;
	n = lo | (hi << 8);
	if (n > PW_MAX_PAYLOAD)
		goto fail;
	for (i = 0; i < n; i++)
		if (!sio_xfer(0, &resp[i]))
			goto fail;
	if (!sio_xfer(0, &lo) || !sio_xfer(0, &hi))
		goto fail;
	sio_deselect();

	crc = pw_crc16(PW_CRC16_INIT, (const uint8_t[]){ (uint8_t)n, (uint8_t)(n >> 8) }, 2);
	crc = pw_crc16(crc, resp, (size_t)n);
	if (crc != (uint16_t)(lo | (hi << 8)))
		return -2;
	*len = n;
	return PW_ST_READY;
fail:
	sio_deselect();
	link_bus_errors++;
	return -1;
}

int link_call(uint8_t op, const uint8_t *args, int alen,
              uint8_t *data, int dmax, int *dlen, uint32_t timeout_ms) {
	const uint32_t start = plat_ms();
	uint32_t contact = start;        /* last time the Pico answered at all */
	int need_send = 1, len;

	if (alen > PW_MAX_PAYLOAD - 2)
		return LINK_ERR_PROTO;
	seq++;
	req[0] = op;
	req[1] = seq;
	if (alen)
		memcpy(req + 2, args, (size_t)alen);

	for (;;) {
		const uint32_t now = plat_ms();
		if (now - start > timeout_ms || now - contact > LINK_DEAD_MS)
			return LINK_ERR_TIMEOUT;

		if (need_send) {
			int r = send_request(req, alen + 2);
			if (r >= 0)
				contact = plat_ms();
			if (r == 1)
				need_send = 0;
			else
				wait_a_bit();
			continue;
		}

		int r = read_response(&len);
		if (r != -1)
			contact = plat_ms();
		if (r == PW_ST_READY) {
			if (len >= 3 && resp[0] == op && resp[1] == seq) {
				int n = len - 3;
				if (n > dmax)
					n = dmax;
				if (data && n > 0)
					memcpy(data, resp + 3, (size_t)n);
				if (dlen)
					*dlen = n;
				return resp[2];
			}
			need_send = 1;           /* an older answer: our request was lost */
			link_retries++;
		} else if (r == PW_ST_NONE) {
			need_send = 1;           /* request lost (bad CRC on the way) */
			link_retries++;
		} else {
			wait_a_bit();            /* busy, bus error or bad CRC: ask again */
		}
	}
}
