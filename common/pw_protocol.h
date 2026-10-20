/*
 * pw_protocol.h - the PS1 <-> Pico protocol of ps1-writer.
 * Shared by the PS1 program, the Pico firmware and the PC tests.
 *
 * The PS1 is the bus master (controller port 2). Every transaction:
 *   PS1 pulls ATT low, sends PW_DEV_ADDR, then a command byte, then more
 *   bytes; the Pico answers each byte at the same time (full duplex).
 *   The Pico pulses ACK after every byte of its own transactions, the last
 *   one included; the PS1 simply deselects when it has what it needs.
 *
 * ---------------------------------------------------------------------------
 * PW_CMD_KEYS - read keyboard events (polled every frame)
 *
 *   PS1:  ADDR  KEYS  00     00   00 ...
 *   Pico: --    --    flags  n    ev0.kind ev0.code ev0.mods  ev1...
 *
 *   n <= PW_MAX_EVENTS. The events are removed from the Pico's queue only
 *   after the PS1 has read the last byte, so an interrupted transfer
 *   neither loses nor repeats keys.
 *
 * ---------------------------------------------------------------------------
 * PW_CMD_REQUEST - send a request (file operations etc.)
 *
 *   PS1:  ADDR  REQ  lenL    lenH  payload...  crcL crcH
 *   Pico: --    --   status  --    --          --   --
 *
 *   status = PW_ST_READY: request accepted, carry on.
 *            anything else: the Pico is busy; the PS1 stops and retries.
 *   crc = pw_crc16 over lenL, lenH and the payload. A request with a bad
 *   CRC is dropped; the PS1 notices because no response with its sequence
 *   number appears, and sends it again.
 *
 * ---------------------------------------------------------------------------
 * PW_CMD_RESPONSE - fetch the answer to the last request
 *
 *   PS1:  ADDR  RESP  00      00    00    00...      00   00
 *   Pico: --    --    status  lenL  lenH  payload..  crcL crcH
 *
 *   status = PW_ST_READY  -> length, payload and crc follow
 *            PW_ST_BUSY   -> still working, ask again (no more bytes)
 *            PW_ST_NONE   -> no request pending (no more bytes)
 *   The response stays available until the next request is accepted, so
 *   the PS1 can read it again if the CRC was wrong.
 *
 * ---------------------------------------------------------------------------
 * Request payload:  [op][seq][arguments...]
 * Response payload: [op][seq][result][data...]
 * Numbers are little endian. Text is ISO-8859-1 (Latin-1) on the PS1 side;
 * files on the SD card are UTF-8 and the Pico converts.
 */

#ifndef PW_PROTOCOL_H
#define PW_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define PW_PROTOCOL_VERSION 2

#define PW_DEV_ADDR      0x22      /* the video project uses 0x21 */

#define PW_CMD_KEYS      0x01
#define PW_CMD_REQUEST   0x10
#define PW_CMD_RESPONSE  0x11

#define PW_ST_READY      0xA5
#define PW_ST_BUSY       0x01
#define PW_ST_NONE       0x00

#define PW_MAX_EVENTS    16
#define PW_MAX_PAYLOAD   2048      /* request and response payload limit */
#define PW_READ_CHUNK    2000      /* data bytes per READ response */
#define PW_SAVE_CHUNK    1900      /* op bytes per SAVE_DATA request */

/* KEYS flags */
#define PW_FLAG_KEYBOARD 0x01      /* a keyboard is connected */
#define PW_FLAG_SD       0x02      /* SD card mounted */
#define PW_FLAG_AI       0x04      /* the PC's AI bridge is connected (USB) */

/* ---- keyboard events: 3 bytes each ---- */
#define PW_EV_CHAR       1         /* code = Latin-1 character */
#define PW_EV_KEY        2         /* code = PW_KEY_* */
#define PW_EV_CTRL       3         /* code = lowercase letter, Ctrl held */

#define PW_MOD_CTRL      0x01
#define PW_MOD_SHIFT     0x02
#define PW_MOD_ALT       0x04
#define PW_MOD_ALTGR     0x08

enum {
	PW_KEY_UP = 1, PW_KEY_DOWN, PW_KEY_LEFT, PW_KEY_RIGHT,
	PW_KEY_HOME, PW_KEY_END, PW_KEY_PGUP, PW_KEY_PGDN,
	PW_KEY_BACKSPACE, PW_KEY_DELETE, PW_KEY_ENTER, PW_KEY_TAB,
	PW_KEY_ESC, PW_KEY_INSERT,
	PW_KEY_F1 = 0x40              /* F1..F12 = 0x40..0x4B */
};

/* ---- request operations ---- */
enum {
	PW_OP_HELLO      = 0x01,  /* -> [version][flags][max payload u16] */
	PW_OP_LIST       = 0x02,  /* [start u16] -> [total u16][count] then per file:
	                             [name len][name][size u32] */
	PW_OP_OPEN       = 0x03,  /* [name] (empty = last used) -> [size u32][name len][name] */
	PW_OP_READ       = 0x04,  /* [offset u32][len u16] -> data */
	PW_OP_CREATE     = 0x05,  /* [name] -> new empty file, becomes the open file */
	PW_OP_SAVE_BEGIN = 0x06,  /* -> starts a save of the open file */
	PW_OP_SAVE_DATA  = 0x07,  /* [chunk u16][save ops...] */
	PW_OP_SAVE_END   = 0x08,  /* [size u32][crc32 u32] -> [size u32] */
	PW_OP_CONFIG_GET = 0x09,  /* -> [settings bytes] (empty if none saved) */
	PW_OP_CONFIG_SET = 0x0A,  /* [settings bytes], up to PW_CONFIG_MAX */

	/* AI link: the Pico passes questions to a program on the PC (USB) */
	PW_OP_AI_ASK     = 0x20,  /* [cursor paragraph u32][cursor offset u32][question]
	                             sends the question and the saved document */
	PW_OP_AI_POLL    = 0x21,  /* -> [PW_AI_* state][next piece of the answer] */
	PW_OP_AI_CANCEL  = 0x22,  /* forget the current question */
};

#define PW_CONFIG_MAX    64
#define PW_AI_MAX_QUESTION 1000   /* bytes of question text */
#define PW_AI_CHUNK      1900     /* answer bytes per AI_POLL */

/* AI_POLL states */
enum {
	PW_AI_IDLE = 0,           /* no question pending */
	PW_AI_MORE,               /* answer text follows (maybe none yet); poll again */
	PW_AI_DONE,               /* this is the last piece of the answer */
	PW_AI_ERROR,              /* the text is an error message */
};

/* save ops inside SAVE_DATA: the new file = a list of pieces */
#define PW_SAVE_KEEP     0x01  /* [off u32][len u32] copy from the current file */
#define PW_SAVE_BYTES    0x02  /* [len u16][bytes] new text */

/* results */
enum {
	PW_OK = 0,
	PW_ERR_NOT_FOUND,
	PW_ERR_EXISTS,
	PW_ERR_IO,
	PW_ERR_NO_SD,
	PW_ERR_BAD_ARGS,
	PW_ERR_BAD_STATE,
	PW_ERR_VERIFY,
	PW_ERR_NOT_AVAILABLE,
	PW_ERR_TOO_BIG,
};

#define PW_MAX_NAME      48       /* file name length in bytes (Latin-1) */

/* ---- helpers ---- */

/* CRC-16/CCITT-FALSE */
static inline uint16_t pw_crc16(uint16_t crc, const uint8_t *p, size_t n) {
	while (n--) {
		crc ^= (uint16_t)(*p++) << 8;
		for (int i = 0; i < 8; i++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc;
}
#define PW_CRC16_INIT 0xFFFF

/* CRC-32 (IEEE), bitwise: small, fast enough for a few hundred KB */
static inline uint32_t pw_crc32(uint32_t crc, const uint8_t *p, size_t n) {
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static inline void pw_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void pw_put32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t pw_get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t pw_get32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif
