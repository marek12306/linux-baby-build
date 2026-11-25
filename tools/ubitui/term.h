/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UBITUI_TERM_H
#define UBITUI_TERM_H

#include <stdint.h>

enum {
	KEY_NONE = -1,
	KEY_TAB = 9,
	KEY_ENTER = 13,
	KEY_ESC = 27,
	KEY_BACKSPACE = 127,
	KEY_UP = 0x100,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_HOME,
	KEY_END,
	KEY_PGUP,
	KEY_PGDN,
	KEY_INS,
	KEY_DEL,
	KEY_BTAB,
	KEY_F1,
	KEY_F2,
	KEY_F3,
	KEY_F4,
	KEY_F5,
	KEY_F6,
	KEY_F7,
	KEY_F8,
	KEY_F9,
	KEY_F10,
	KEY_RESIZE,
	KEY_HANGUP,
};

#define KEY_CTRL(c) ((c) & 0x1f)

/* Cell attributes: 4-bit foreground and background (0 = terminal default). */
enum { C_BLACK = 1, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN, C_WHITE };
#define A_FG(c) ((uint16_t)(c))
#define A_BG(c) ((uint16_t)((c) << 4))
#define A_BOLD 0x100
#define A_REV 0x200
#define A_UNDER 0x400
#define A_ACS 0x800 /* the character is a DEC line-drawing code below */

/* DEC special graphics codes, drawn as ASCII with -a */
#define ACS_HLINE 'q'
#define ACS_VLINE 'x'
#define ACS_ULCORNER 'l'
#define ACS_URCORNER 'k'
#define ACS_LLCORNER 'm'
#define ACS_LRCORNER 'j'
#define ACS_LTEE 't'
#define ACS_RTEE 'u'
#define ACS_CKBOARD 'a'

extern int scr_rows, scr_cols;
extern int term_mono;

int term_open(int ascii, int mono);
void term_close(void);
/* Next key; KEY_NONE after timeout_ms (negative: wait forever). */
int term_getkey(int timeout_ms);
/* Repaint everything on the next flush; re-probe the size on a serial line. */
void term_redraw(void);

void scr_clear(void);
void scr_fill(int y, int x, int n, uint16_t attr, int ch);
/* Draws at most n characters of s (n < 0: all), padding with spaces to n. */
void scr_putn(int y, int x, uint16_t attr, const char *s, int n);
void scr_put(int y, int x, uint16_t attr, const char *s);
void scr_printf(int y, int x, uint16_t attr, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));
void scr_acs(int y, int x, uint16_t attr, int acs);
void scr_hline(int y, int x, int n, uint16_t attr);
void scr_box(int y, int x, int h, int w, uint16_t attr);
/* Shows the hardware cursor there on the next flush (y < 0 hides it). */
void scr_cursor(int y, int x);
void scr_flush(void);

#endif
