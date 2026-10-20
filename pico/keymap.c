/*
 * keymap.c - USB keyboard (US layout) -> characters and editing keys.
 *
 * HID usage codes: a = 0x04 ... z = 0x1D, 1 = 0x1E ... 0 = 0x27, etc.
 * Modifier byte: bit0 LCtrl, 1 LShift, 2 LAlt, 3 LGUI,
 *                bit4 RCtrl, 5 RShift, 6 RAlt (AltGr), 7 RGUI.
 */

#include <string.h>
#include "keymap.h"
#include "pw_protocol.h"

#define REPEAT_DELAY_MS 500
#define REPEAT_RATE_MS  33

/* a character table entry: normal, shift; 0 = nothing */
typedef struct { uint8_t n, s; } keychars_t;

static const keychars_t us_keys[0x39] = {
	[0x1E] = { '1', '!' }, [0x1F] = { '2', '@' }, [0x20] = { '3', '#' },
	[0x21] = { '4', '$' }, [0x22] = { '5', '%' }, [0x23] = { '6', '^' },
	[0x24] = { '7', '&' }, [0x25] = { '8', '*' }, [0x26] = { '9', '(' },
	[0x27] = { '0', ')' }, [0x2C] = { ' ', ' ' },
	[0x2D] = { '-', '_' }, [0x2E] = { '=', '+' },
	[0x2F] = { '[', '{' }, [0x30] = { ']', '}' },
	[0x31] = { '\\', '|' }, [0x32] = { '\\', '|' },
	[0x33] = { ';', ':' }, [0x34] = { '\'', '"' },
	[0x35] = { '`', '~' }, [0x36] = { ',', '<' },
	[0x37] = { '.', '>' }, [0x38] = { '/', '?' },
};

static keymap_emit_fn emit;
static uint8_t prev_keys[6];
static int caps;

/* repeat state */
static uint8_t  rep_usage, rep_mods;
static uint32_t rep_next;

void keymap_init(keymap_emit_fn fn) {
	emit = fn;
	keymap_reset();
}

void keymap_reset(void) {
	memset(prev_keys, 0, sizeof prev_keys);
	rep_usage = 0;
	caps = 0;
}

static uint8_t mods_of(uint8_t m) {
	uint8_t r = 0;
	if (m & 0x11) r |= PW_MOD_CTRL;
	if (m & 0x22) r |= PW_MOD_SHIFT;
	if (m & 0x04) r |= PW_MOD_ALT;
	if (m & 0x40) r |= PW_MOD_ALTGR;
	return r;
}

static int special_key(uint8_t u) {
	switch (u) {
	case 0x28: case 0x58: return PW_KEY_ENTER;
	case 0x29: return PW_KEY_ESC;
	case 0x2A: return PW_KEY_BACKSPACE;
	case 0x2B: return PW_KEY_TAB;
	case 0x49: return PW_KEY_INSERT;
	case 0x4A: return PW_KEY_HOME;
	case 0x4B: return PW_KEY_PGUP;
	case 0x4C: return PW_KEY_DELETE;
	case 0x4D: return PW_KEY_END;
	case 0x4E: return PW_KEY_PGDN;
	case 0x4F: return PW_KEY_RIGHT;
	case 0x50: return PW_KEY_LEFT;
	case 0x51: return PW_KEY_DOWN;
	case 0x52: return PW_KEY_UP;
	}
	if (u >= 0x3A && u <= 0x45)
		return PW_KEY_F1 + (u - 0x3A);
	return 0;
}

/* the character for a key, or 0 */
static uint8_t key_char(uint8_t u, uint8_t m) {
	const int shift = (m & PW_MOD_SHIFT) != 0;

	if (m & PW_MOD_ALTGR)
		return 0;
	if (u >= 0x04 && u <= 0x1D) {               /* letters */
		char c = (char)('a' + (u - 0x04));
		return (uint8_t)((shift ^ caps) ? c - 32 : c);
	}
	if (u >= 0x59 && u <= 0x63) {               /* keypad, NumLock on */
		static const char kp[] = "1234567890.";
		return (uint8_t)kp[u - 0x59];
	}
	switch (u) {
	case 0x54: return '/';
	case 0x55: return '*';
	case 0x56: return '-';
	case 0x57: return '+';
	case 0x64: return shift ? '|' : '\\';       /* extra key on ISO boards */
	}
	if (u < 0x39)
		return shift ? us_keys[u].s : us_keys[u].n;
	return 0;
}

/* emit the event(s) for one key press; returns 1 if the key may repeat */
static int press(uint8_t u, uint8_t m) {
	if (u == 0x39) {                            /* caps lock */
		caps ^= 1;
		return 0;
	}
	int sp = special_key(u);
	if (sp) {
		emit(PW_EV_KEY, (uint8_t)sp, m);
		return 1;
	}
	if ((m & PW_MOD_CTRL) && !(m & PW_MOD_ALTGR)) {
		if (u >= 0x04 && u <= 0x1D) {           /* Ctrl+letter, by key position */
			emit(PW_EV_CTRL, (uint8_t)('a' + (u - 0x04)), m);
			return 0;
		}
		return 0;
	}
	uint8_t c = key_char(u, m);
	if (c) {
		emit(PW_EV_CHAR, c, m);
		return 1;
	}
	return 0;
}

void keymap_report(const uint8_t *r, int len, uint32_t now) {
	if (len < 8)
		return;
	const uint8_t m = mods_of(r[0]);
	const uint8_t *keys = r + 2;

	if (keys[0] == 0x01)                        /* rollover error: ignore */
		return;

	/* key held for repeat released? */
	if (rep_usage) {
		int held = 0;
		for (int i = 0; i < 6; i++)
			if (keys[i] == rep_usage)
				held = 1;
		if (!held)
			rep_usage = 0;
	}

	/* newly pressed keys */
	for (int i = 0; i < 6; i++) {
		uint8_t u = keys[i];
		if (!u)
			continue;
		int was = 0;
		for (int j = 0; j < 6; j++)
			if (prev_keys[j] == u)
				was = 1;
		if (was)
			continue;
		if (press(u, m)) {
			rep_usage = u;
			rep_mods = m;
			rep_next = now + REPEAT_DELAY_MS;
		}
	}
	if (rep_usage)
		rep_mods = m;                           /* shift may change while held */
	memcpy(prev_keys, keys, 6);
}

void keymap_tick(uint32_t now) {
	if (rep_usage && (int32_t)(now - rep_next) >= 0) {
		press(rep_usage, rep_mods);
		rep_next = now + REPEAT_RATE_MS;
	}
}
