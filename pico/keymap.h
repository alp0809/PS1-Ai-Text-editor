/*
 * keymap.h - USB HID boot keyboard reports -> PS1 key events.
 *
 * Pure C (no USB code), so it can be tested on a PC.
 * US layout. Key repeat: 500 ms delay, then about 30 characters per
 * second.
 */

#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdint.h>

/* where events go: return 0 if the queue is full */
typedef int (*keymap_emit_fn)(uint8_t kind, uint8_t code, uint8_t mods);

void keymap_init(keymap_emit_fn emit);

/* an 8-byte boot keyboard report: [mods][reserved][6 key codes] */
void keymap_report(const uint8_t *report, int len, uint32_t now_ms);

/* call often: produces repeat events while a key is held */
void keymap_tick(uint32_t now_ms);

/* keyboard unplugged */
void keymap_reset(void);

#endif
