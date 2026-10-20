/*
 * charset.h - ISO-8859-1 (Latin-1) <-> Unicode / UTF-8.
 *
 * The PS1 works in Latin-1: one byte per character, and the byte value is
 * the Unicode code point. Files on the SD card are UTF-8 so they open
 * normally on a PC; the Pico converts in both directions.
 */

#ifndef CHARSET_H
#define CHARSET_H

#include <stdint.h>

static inline uint32_t latin1_to_unicode(uint8_t c) { return c; }

/* Returns the Latin-1 byte, or -1 if the character has no Latin-1 form. */
static inline int unicode_to_latin1(uint32_t u) {
	return u < 0x100 ? (int)u : -1;
}

/* Close ASCII stand-ins for common characters Latin-1 lacks (typographic
 * quotes, dashes, ellipsis, a few accented letters...). NULL if none. */
static inline const char *unicode_fallback(uint32_t u) {
	switch (u) {
	case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
	case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
	case 0x2013: case 0x2014: case 0x2212: return "-";
	case 0x2026: return "...";
	case 0x2022: return "*";
	case 0x2009: case 0x202F: return " ";
	case 0x20AC: return "EUR";
	case 0x0152: return "OE";
	case 0x0153: return "oe";
	case 0x011E: return "G";     /* g-breve, dotless i... */
	case 0x011F: return "g";
	case 0x0130: return "I";
	case 0x0131: return "i";
	case 0x015E: return "S";
	case 0x015F: return "s";
	case 0xFEFF: return "";      /* byte order mark */
	}
	return 0;
}

/* Encode one code point as UTF-8; returns the number of bytes (1-4). */
static inline int utf8_encode(uint32_t u, uint8_t *out) {
	if (u < 0x80)    { out[0] = (uint8_t)u; return 1; }
	if (u < 0x800)   { out[0] = (uint8_t)(0xC0 | (u >> 6));
	                   out[1] = (uint8_t)(0x80 | (u & 0x3F)); return 2; }
	if (u < 0x10000) { out[0] = (uint8_t)(0xE0 | (u >> 12));
	                   out[1] = (uint8_t)(0x80 | ((u >> 6) & 0x3F));
	                   out[2] = (uint8_t)(0x80 | (u & 0x3F)); return 3; }
	out[0] = (uint8_t)(0xF0 | (u >> 18));
	out[1] = (uint8_t)(0x80 | ((u >> 12) & 0x3F));
	out[2] = (uint8_t)(0x80 | ((u >> 6) & 0x3F));
	out[3] = (uint8_t)(0x80 | (u & 0x3F));
	return 4;
}

/* Streaming UTF-8 decoder: feed bytes one at a time.
 * Returns 1 and sets *out when a code point is complete, 0 when more bytes
 * are needed, -1 for an invalid sequence (*out = U+FFFD; the byte that broke
 * the sequence is lost, which is acceptable for text). */
typedef struct { uint32_t cp; int need; } utf8_dec_t;

static inline int utf8_decode(utf8_dec_t *d, uint8_t b, uint32_t *out) {
	if (d->need == 0) {
		if (b < 0x80)                { *out = b; return 1; }
		else if ((b & 0xE0) == 0xC0) { d->cp = b & 0x1F; d->need = 1; }
		else if ((b & 0xF0) == 0xE0) { d->cp = b & 0x0F; d->need = 2; }
		else if ((b & 0xF8) == 0xF0) { d->cp = b & 0x07; d->need = 3; }
		else                         { *out = 0xFFFD; return -1; }
		return 0;
	}
	if ((b & 0xC0) != 0x80) {
		d->need = 0;
		*out = 0xFFFD;
		return -1;
	}
	d->cp = (d->cp << 6) | (b & 0x3F);
	if (--d->need == 0) {
		*out = d->cp;
		return 1;
	}
	return 0;
}

#endif
