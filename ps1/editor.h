/*
 * editor.h - the text editor: keys in, a screen of character cells out.
 * Portable C (no PS1 headers), so the PC tests run the same code.
 */

#ifndef EDITOR_H
#define EDITOR_H

#include <stdint.h>
#include "link.h"
#include "doc.h"

#define ED_COLS          80
#define ED_MAX_ROWS      30      /* 30 x 8 pixels = the whole 240-line screen */
#define ED_MIN_ROWS      20
#define ED_DEFAULT_ROWS  28      /* a little margin: many TVs and converters
                                    cut off the top and bottom lines */

/* cell attributes (the renderer picks the colours) */
enum {
	A_TEXT = 0,
	A_HEAD,          /* Markdown heading line ("# ...") */
	A_STATUS,
	A_WARN,          /* status bar message that needs attention */
	A_DLG,
	A_DLG_SEL,
	A_DLG_TITLE,
};

typedef struct {
	uint8_t ch, attr;
} cell_t;

extern cell_t ed_screen[ED_MAX_ROWS][ED_COLS];
extern int    ed_rows;               /* rows in use (F11 / F12 change it) */
extern int    ed_cur_x, ed_cur_y;    /* cursor cell, -1 = hidden */
extern int    ed_overwrite;          /* 1 = overwrite mode (block cursor) */
extern doc_t  ed_doc;

/* present(): show ed_screen now (used during long loads and saves) */
void ed_init(void (*present)(void));

/* one frame of the main loop: read keys from the Pico, handle them, run
 * the timers, redraw, present(). The PS1's main() just calls this forever. */
void ed_frame(void);

/* the parts of ed_frame, for tests */

/* one keyboard event from the Pico */
void ed_key(const link_event_t *e);

/* call every frame with the latest Pico flags (PW_FLAG_*, plus ED_NO_PICO
 * if the key poll got no answer): start-up, autosave, the AI chat */
#define ED_NO_PICO 0x80
void ed_tick(uint8_t flags);

/* rebuild ed_screen if anything changed; returns 1 if it did */
int  ed_render(void);

#endif
