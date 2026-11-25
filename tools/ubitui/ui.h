/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UBITUI_UI_H
#define UBITUI_UI_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "term.h"

enum style {
	S_NORMAL,
	S_TITLE,
	S_HEADER,
	S_SEL,
	S_DIM,
	S_WARN,
	S_ERR,
	S_OK,
	S_STAGED,
	S_HOT,
	S_HOT_SEL,
	S_BORDER,
	S_FIELD,
	S_FIELD_FOCUS,
	S_BUTTON_SEL,
	S_MAP1, /* map/bar palette, S_MAP1..S_MAP6 */
	S_MAP2,
	S_MAP3,
	S_MAP4,
	S_MAP5,
	S_MAP6,
	S_NSTYLES
};

uint16_t st(int style);

/* Layers: the main screen and every open dialog register a draw function,
 * so any dialog can repaint the whole stack (resize, Ctrl-L). */
typedef void (*draw_fn)(void *ctx);
void ui_push(draw_fn fn, void *ctx);
void ui_pop(void);
void ui_redraw(void);
/* term_getkey() plus the keys every loop handles the same way: Ctrl-L. */
int ui_getkey(int timeout_ms);

/* Centered dialog frame: clears it, draws the border and the title. */
void ui_frame(int h, int w, const char *title, int title_style, int *y, int *x);

void ui_msg(const char *title, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
void ui_error(const char *title, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
int ui_yesno(const char *title, const char *text, int def_yes);
/* Destructive operations: the user has to type "yes". */
int ui_confirm_yes(const char *title, const char *text);

struct list_item {
	const char *text;
	const char *help;
	int disabled;
	int hotkey;
};
int ui_list(const char *title, const char *intro, const struct list_item *it,
	    int n, int sel);

enum ftype { F_TEXT, F_CHOICE, F_CHECK };

struct field {
	enum ftype type;
	const char *label;
	const char *help;
	char buf[256]; /* F_TEXT */
	int maxlen;
	const char *const *choices; /* F_CHOICE, NULL-terminated */
	int choice;
	int checked; /* F_CHECK */
	int hidden;
	int browse; /* F_TEXT: F2 / Ctrl-B opens the file browser */
	int pos, off; /* editing state */
};

/* Validates the form and fills note (shown under the fields). Returns 0 when
 * the form can be submitted. May hide or show fields. */
typedef int (*form_check_fn)(struct field *f, int n, char *note, size_t sz,
			     void *ctx);
/* Returns 1 on OK, 0 on cancel. */
int ui_form(const char *title, const char *intro, struct field *f, int n,
	    form_check_fn check, void *ctx);

/* Text documents with inline style switches, for info views. */
struct doc {
	char **lines;
	int n, cap;
	char *cur;
	size_t curlen, curcap;
};
#define DOC_STYLE '\001'
void doc_style(struct doc *d, int style);
void doc_printf(struct doc *d, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
void doc_free(struct doc *d);
typedef void (*doc_build_fn)(struct doc *d, int width, void *ctx);
void ui_view(const char *title, doc_build_fn build, void *ctx);

struct progress {
	const char *title;
	char line[160];
	long long done, total;
	int cancel_ok;
	int is_bytes; /* set by prog_start; clear for counts */
	struct timespec t0, last;
};
void prog_start(struct progress *p, const char *title, long long total,
		int cancel_ok);
/* Redraws at most every 150 ms. Returns 1 if the user confirmed a cancel. */
int prog_update(struct progress *p, long long done, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void prog_end(struct progress *p);

/* File browser starting at the directory of path; writes the chosen file. */
int ui_browse(char *path, size_t sz);

char *fmt_size(long long bytes, char *buf, size_t sz);
char *fmt_num(long long n, char *buf, size_t sz);

#endif
