/*
 * shared.h - data shared between the Pico's two cores.
 *
 *   core 0 (bus): answers the PS1, byte by byte, within microseconds.
 *   core 1 (everything else): USB keyboard, SD card, file requests.
 *
 * Two hand-over points, both lock-free:
 *   key queue  - core 1 adds keyboard events, core 0 hands them to the PS1.
 *   mailbox    - core 0 stores a request from the PS1, core 1 processes it
 *                and leaves a response that core 0 sends back.
 */

#ifndef SHARED_H
#define SHARED_H

#include <stdint.h>
#include "pw_protocol.h"

#ifdef PICO_BUILD
#include "hardware/sync.h"
#define MEM_BARRIER() __dmb()
#else
#define MEM_BARRIER() __sync_synchronize()
#endif

typedef struct {
	uint8_t kind, code, mods;
} pw_event_t;

/* ---- key queue: single producer (core 1), single consumer (core 0) ---- */
#define KEYQ_SIZE 256                    /* power of two; holds the keys typed
                                            while the PS1 waits for the card */

extern pw_event_t        keyq[KEYQ_SIZE];
extern volatile uint32_t keyq_head;      /* written by core 1 only */
extern volatile uint32_t keyq_tail;      /* written by core 0 only */

/* returns 0 if the queue is full (the event is dropped) */
int keyq_push(uint8_t kind, uint8_t code, uint8_t mods);

/* ---- status flags shown to the PS1 (PW_FLAG_*) ---- */
extern volatile uint8_t pw_flags;

/* ---- request / response mailbox ---- */
enum {
	MB_IDLE = 0,       /* core 0 may accept a new request          */
	MB_PENDING,        /* request stored, waiting for core 1        */
	MB_PROCESSING,     /* core 1 is working on it                   */
};

extern volatile int      mb_state;
extern volatile int      mb_resp_valid;  /* response buffer holds an answer */
extern uint8_t           mb_req[PW_MAX_PAYLOAD];
extern volatile uint16_t mb_req_len;
extern uint8_t           mb_resp[PW_MAX_PAYLOAD];
extern volatile uint16_t mb_resp_len;

void shared_reset(void);

#endif
