/*
 * link.h - PS1 side of the protocol: key polling and request/response calls
 * with retries. Portable C on top of sio.h; the platform provides plat_ms()
 * and plat_idle().
 */

#ifndef LINK_H
#define LINK_H

#include <stdint.h>
#include "pw_protocol.h"

uint32_t plat_ms(void);      /* milliseconds since start */
void     plat_idle(void);    /* short pause while waiting for the Pico */

typedef struct {
	uint8_t kind, code, mods;
} link_event_t;

#define LINK_ERR_TIMEOUT  (-100)  /* no answer in time, or no answer at all for LINK_DEAD_MS */
#define LINK_ERR_PROTO    (-101)

/* Read up to max key events. Returns the count, or -1 if the Pico didn't
 * answer. *flags gets the PW_FLAG_* status bits. */
int link_keys(link_event_t *ev, int max, uint8_t *flags);

#define LINK_DEAD_MS      3000

/* One request. Returns the result code (PW_OK = 0, PW_ERR_* > 0) or a
 * LINK_ERR_* (< 0). Response data (after the result byte) goes to data. */
int link_call(uint8_t op, const uint8_t *args, int alen,
              uint8_t *data, int dmax, int *dlen, uint32_t timeout_ms);

/* if set, called while link_call waits for the Pico (between polls) */
extern void (*link_wait_hook)(void);

/* statistics, for the curious */
extern uint32_t link_retries, link_bus_errors;

#endif
