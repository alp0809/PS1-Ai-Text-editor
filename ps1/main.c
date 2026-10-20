/*
 * ps1-writer - PlayStation side
 *
 * A text editor for long documents. Keys come from a USB keyboard through
 * the Pico on controller port 2; documents live on the Pico's SD card.
 *
 * This file is the PS1-specific part: the screen (640x240, 80 columns and
 * up to 30 rows of 8x8 cells, one textured sprite per character), the
 * clock, and the main loop. The editor itself (editor.c, doc.c, link.c)
 * is plain C and is tested on a PC.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psxgpu.h>

#include "editor.h"
#include "link.h"
#include "sio.h"
#include "font_latin1.h"

#define SCREEN_W  640
#define SCREEN_H  240
#define CELL      8
#define MARGIN_X  ((SCREEN_W - ED_COLS * CELL) / 2)
#define MARGIN_Y  ((SCREEN_H - ed_rows * CELL) / 2)   /* ed_rows: F11 / F12 */

/* the font texture (4 bits per pixel, 16x16 glyphs = 128x128 pixels) sits
 * right of the two screen buffers in VRAM */
#define FONT_X    640
#define FONT_Y    0
#define CLUT_X    640
#define CLUT_Y    256

#define STACK_RESERVE 0x10000      /* keep 64 KB between the heap and the stack */

/* ---------------------------------------------------------------- colours */

typedef struct { uint8_t r, g, b; } rgb_t;

static const rgb_t BG = { 16, 24, 48 };          /* screen background */

static const struct { rgb_t fg; rgb_t bg; uint8_t has_bg; } colours[] = {
	[A_TEXT]      = { { 220, 220, 210 }, {   0,   0,   0 }, 0 },
	[A_HEAD]      = { { 255, 210,  90 }, {   0,   0,   0 }, 0 },
	[A_STATUS]    = { {  10,  20,  40 }, { 150, 180, 200 }, 1 },
	[A_WARN]      = { { 255, 255, 255 }, { 170,  30,  30 }, 1 },
	[A_DLG]       = { { 230, 230, 230 }, {  50,  60,  90 }, 1 },
	[A_DLG_SEL]   = { {  10,  20,  40 }, { 230, 200, 100 }, 1 },
	[A_DLG_TITLE] = { { 255, 220, 120 }, {  50,  60,  90 }, 1 },
};

/* ---------------------------------------------------------------- screen */

/* one sprite per cell at most, plus background runs and the cursor */
#define PRIM_BYTES (ED_MAX_ROWS * ED_COLS * sizeof(SPRT_8) + ED_MAX_ROWS * 24 * sizeof(TILE))

typedef struct {
	DISPENV  disp;
	DRAWENV  draw;
	uint32_t ot[2];             /* [1] backgrounds, [0] text (drawn last) */
	uint8_t  prims[PRIM_BYTES];
} framebuf_t;

static framebuf_t fb[2];
static int        cur;          /* the buffer being built */
static uint8_t   *nextp, *endp;
static uint32_t   frame_no;

static void *new_prim(size_t size) {
	if (nextp + size > endp)
		return 0;
	void *p = nextp;
	nextp += size;
	return p;
}

static void tile(int slot, int x, int y, int w, int h, rgb_t c) {
	TILE *t = new_prim(sizeof(TILE));
	if (!t)
		return;
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, c.r, c.g, c.b);
	addPrim(&fb[cur].ot[slot], t);
}

static void glyph(int x, int y, uint8_t ch, rgb_t c) {
	SPRT_8 *s = new_prim(sizeof(SPRT_8));
	if (!s)
		return;
	setSprt8(s);
	setXY0(s, x, y);
	setUV0(s, (ch & 15) * CELL, (ch >> 4) * CELL);
	s->clut = getClut(CLUT_X, CLUT_Y);
	/* the texture is white; 128 = full brightness, so halve the colour */
	setRGB0(s, c.r >> 1, c.g >> 1, c.b >> 1);
	addPrim(&fb[cur].ot[0], s);
}

static void font_upload(void) {
	static uint16_t tex[128 * 128 / 4];        /* 4 pixels per halfword */
	static uint16_t clut[16];
	memset(tex, 0, sizeof tex);
	for (int c = 0; c < 256; c++) {
		const int gx = (c & 15) * CELL, gy = (c >> 4) * CELL;
		for (int row = 0; row < CELL; row++) {
			const uint8_t bits = font_latin1[c][row];
			for (int col = 0; col < CELL; col++) {
				if (!(bits & (1 << col)))
					continue;
				const int px = gx + col, py = gy + row;
				tex[py * 32 + px / 4] |= (uint16_t)(1u << ((px & 3) * 4));
			}
		}
	}
	clut[0] = 0x0000;                          /* transparent */
	clut[1] = 0x7FFF;                          /* white */

	RECT r = { FONT_X, FONT_Y, 128 / 4, 128 };
	LoadImage(&r, (const uint32_t *)tex);
	RECT rc = { CLUT_X, CLUT_Y, 16, 1 };
	LoadImage(&rc, (const uint32_t *)clut);
	DrawSync(0);
}

static void screen_init(void) {
	ResetGraph(0);
	for (int i = 0; i < 2; i++) {
		/* show one half of VRAM while drawing into the other */
		SetDefDispEnv(&fb[i].disp, 0, i * SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&fb[i].draw, 0, (1 - i) * SCREEN_H, SCREEN_W, SCREEN_H);
		fb[i].draw.isbg = 1;
		setRGB0(&fb[i].draw, BG.r, BG.g, BG.b);
		fb[i].draw.tpage = getTPage(0, 0, FONT_X, FONT_Y);   /* 4-bit font */
	}
	font_upload();
	cur = 0;
}

/* build the frame from ed_screen and show it */
static void draw_frame(void) {
	framebuf_t *f = &fb[cur];
	ClearOTagR(f->ot, 2);
	nextp = f->prims;
	endp = f->prims + sizeof f->prims;

	const int blink_on = (frame_no / 20) & 1 ? 0 : 1;

	for (int y = 0; y < ed_rows; y++) {
		const int py = MARGIN_Y + y * CELL;

		/* backgrounds: one tile per run of cells with the same attribute */
		for (int x = 0; x < ED_COLS;) {
			const int a = ed_screen[y][x].attr;
			int e = x + 1;
			while (e < ED_COLS && ed_screen[y][e].attr == a)
				e++;
			if (colours[a].has_bg)
				tile(1, MARGIN_X + x * CELL, py, (e - x) * CELL, CELL, colours[a].bg);
			x = e;
		}

		for (int x = 0; x < ED_COLS; x++) {
			const cell_t c = ed_screen[y][x];
			const int is_cursor = blink_on && x == ed_cur_x && y == ed_cur_y;
			const int px = MARGIN_X + x * CELL;
			rgb_t fg = colours[c.attr].fg;

			if (is_cursor && ed_overwrite) {
				/* block cursor: the letter in the background colour */
				tile(1, px, py, CELL, CELL, fg);
				fg = colours[c.attr].has_bg ? colours[c.attr].bg : BG;
			} else if (is_cursor) {
				/* underline cursor, drawn after the text */
				tile(0, px, py + CELL - 2, CELL, 2, fg);
			}
			if (c.ch != ' ')
				glyph(px, py, c.ch, fg);
		}
	}

	DrawSync(0);
	VSync(0);
	PutDispEnv(&f->disp);
	PutDrawEnv(&f->draw);
	DrawOTag(&f->ot[1]);
	SetDispMask(1);
	cur ^= 1;
	frame_no++;
}

/* the editor calls this during long loads and saves */
static void present(void) {
	draw_frame();
}

/* ---------------------------------------------------------------- platform */

uint32_t plat_ms(void) {
	const uint32_t hz = (GetVideoMode() == MODE_PAL) ? 50 : 60;
	const uint32_t v = (uint32_t)VSync(-1);
	return (v / hz) * 1000u + (v % hz) * 1000u / hz;
}

void plat_idle(void) {
	for (volatile int i = 0; i < 1500; i++);   /* ~0.2 ms */
}

/* ---------------------------------------------------------------- main */

extern uint8_t _end[];

int main(void) {
	/* the heap starts after the program; stop it short of the stack, which
	 * grows down from the top of the 2 MB */
	uint8_t *heap = _end + 4;
	uint8_t *limit = (uint8_t *)(0x80200000 - STACK_RESERVE);
	InitHeap(heap, (size_t)(limit - heap));

	screen_init();
	sio_init();
	ed_init(present);
	for (;;)
		ed_frame();             /* keys, editing, saving, drawing */
}
