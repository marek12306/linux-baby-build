// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Dialogs of ubitui: messages, confirmations, lists, forms, a scrollable
 * document view, progress and a file browser. Every dialog registers a draw
 * function on the layer stack, so a resize repaints the whole stack.
 */
#include "ui.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_LAYERS 16

static struct {
	draw_fn fn;
	void *ctx;
} layers[MAX_LAYERS];
static int nlayers;

uint16_t st(int s)
{
	static const uint16_t color[S_NSTYLES] = {
		[S_NORMAL] = 0,
		[S_TITLE] = A_BOLD,
		[S_HEADER] = A_BOLD,
		[S_SEL] = A_REV,
		[S_DIM] = A_FG(C_CYAN),
		[S_WARN] = A_BOLD | A_FG(C_YELLOW),
		[S_ERR] = A_BOLD | A_FG(C_RED),
		[S_OK] = A_BOLD | A_FG(C_GREEN),
		[S_STAGED] = A_BOLD | A_FG(C_YELLOW),
		[S_HOT] = A_BOLD | A_FG(C_CYAN),
		[S_HOT_SEL] = A_REV | A_BOLD | A_UNDER,
		[S_BORDER] = 0,
		[S_FIELD] = A_UNDER,
		[S_FIELD_FOCUS] = A_REV,
		[S_BUTTON_SEL] = A_REV | A_BOLD,
		[S_MAP1] = A_BG(C_BLUE) | A_FG(C_WHITE),
		[S_MAP2] = A_BG(C_GREEN) | A_FG(C_BLACK),
		[S_MAP3] = A_BG(C_MAGENTA) | A_FG(C_WHITE),
		[S_MAP4] = A_BG(C_CYAN) | A_FG(C_BLACK),
		[S_MAP5] = A_BG(C_YELLOW) | A_FG(C_BLACK),
		[S_MAP6] = A_BG(C_RED) | A_FG(C_WHITE),
	};

	if (term_mono) {
		switch (s) {
		case S_DIM: case S_MAP1: case S_MAP2: case S_MAP3:
		case S_MAP4: case S_MAP5: case S_MAP6:
			return 0;
		case S_HOT: return A_BOLD | A_UNDER;
		case S_WARN: case S_ERR: case S_OK: case S_STAGED:
			return A_BOLD;
		}
	}
	return color[s];
}

void ui_push(draw_fn fn, void *ctx)
{
	if (nlayers < MAX_LAYERS) {
		layers[nlayers].fn = fn;
		layers[nlayers].ctx = ctx;
	}
	nlayers++;
}

void ui_pop(void)
{
	if (nlayers > 0)
		nlayers--;
}

void ui_redraw(void)
{
	int i;

	scr_clear();
	for (i = 0; i < nlayers && i < MAX_LAYERS; i++)
		layers[i].fn(layers[i].ctx);
}

int ui_getkey(int timeout_ms)
{
	int k = term_getkey(timeout_ms);

	if (k == KEY_CTRL('L')) {
		term_redraw();
		return KEY_RESIZE;
	}
	return k;
}

static int imin(int a, int b)
{
	return a < b ? a : b;
}

static int imax(int a, int b)
{
	return a > b ? a : b;
}

void ui_frame(int h, int w, const char *title, int title_style, int *y, int *x)
{
	int i;

	w = imin(w, scr_cols);
	h = imin(h, scr_rows);
	*y = (scr_rows - h) / 2;
	*x = (scr_cols - w) / 2;
	for (i = 0; i < h; i++)
		scr_fill(*y + i, *x, w, 0, ' ');
	scr_box(*y, *x, h, w, st(S_BORDER));
	if (title && *title) {
		char t[256];

		snprintf(t, sizeof(t), " %s ", title);
		scr_putn(*y, *x + 2, st(title_style), t, imin((int)strlen(t), w - 4));
	}
}

/* Greedy word wrap; '\n' breaks. Returns a malloc'ed array of lines. */
static int wrap(const char *text, int width, char ***out)
{
	char **lines = NULL;
	int n = 0, cap = 0;
	const char *p = text;

	if (width < 8)
		width = 8;
	for (;;) {
		const char *nl = strchr(p, '\n');
		int len = nl ? (int)(nl - p) : (int)strlen(p);
		int take = len, skip = 0, i;

		if (len > width) {
			take = width;
			for (i = width; i > width / 3; i--) {
				if (p[i] == ' ') {
					take = i;
					skip = 1;
					break;
				}
			}
		} else if (nl) {
			skip = 1;
		}
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			lines = realloc(lines, cap * sizeof(*lines));
		}
		lines[n++] = strndup(p, take);
		p += take + skip;
		if (take == len && (!nl || !*p))
			break;
	}
	*out = lines;
	return n;
}

static void free_lines(char **l, int n)
{
	while (n-- > 0)
		free(l[n]);
	free(l);
}

/* ---- message / yes-no ---- */

struct msgdlg {
	const char *title;
	int tstyle;
	const char *text;
	int buttons; /* 1: OK, 2: Yes/No */
	int sel;     /* Yes/No: 0 = Yes */
	int top, body, nlines;
};

static void msg_draw(void *ctx)
{
	struct msgdlg *m = ctx;
	int inner = imin(scr_cols - 6, 72), y, x, i, w, h, maxw = 0;
	char **lines;

	m->nlines = wrap(m->text, inner, &lines);
	for (i = 0; i < m->nlines; i++)
		maxw = imax(maxw, strlen(lines[i]));
	w = imax(imax(maxw, (int)strlen(m->title) + 4), 24) + 4;
	m->body = imin(m->nlines, scr_rows - 6);
	if (m->body < 1)
		m->body = 1;
	if (m->top > m->nlines - m->body)
		m->top = imax(0, m->nlines - m->body);
	h = m->body + 4;
	ui_frame(h, w, m->title, m->tstyle, &y, &x);
	for (i = 0; i < m->body && m->top + i < m->nlines; i++)
		scr_putn(y + 1 + i, x + 2, 0, lines[m->top + i], w - 4);
	if (m->nlines > m->body)
		scr_printf(y + h - 1, x + w - 12, st(S_DIM), " %d/%d ",
			   m->top + m->body, m->nlines);
	if (m->buttons == 1) {
		scr_put(y + h - 2, x + (w - 6) / 2, st(S_BUTTON_SEL), "[ OK ]");
	} else {
		int bx = x + (w - 17) / 2;

		scr_put(y + h - 2, bx, st(m->sel == 0 ? S_BUTTON_SEL : S_NORMAL),
			"[ Yes ]");
		scr_put(y + h - 2, bx + 9, st(m->sel == 1 ? S_BUTTON_SEL : S_NORMAL),
			"[ No ]");
	}
	free_lines(lines, m->nlines);
}

static int msg_run(struct msgdlg *m)
{
	int k, ret = -1;

	ui_push(msg_draw, m);
	while (ret < 0) {
		ui_redraw();
		scr_flush();
		k = ui_getkey(-1);
		switch (k) {
		case KEY_UP:
			if (m->top > 0)
				m->top--;
			break;
		case KEY_DOWN:
			if (m->top + m->body < m->nlines)
				m->top++;
			break;
		case KEY_PGUP:
			m->top = imax(0, m->top - m->body);
			break;
		case KEY_PGDN:
			m->top = imax(0, imin(m->nlines - m->body, m->top + m->body));
			break;
		case KEY_LEFT: case KEY_RIGHT: case KEY_TAB: case KEY_BTAB:
			m->sel ^= 1;
			break;
		case KEY_ENTER: case ' ':
			ret = m->buttons == 1 ? 1 : !m->sel;
			break;
		case KEY_ESC: case KEY_HANGUP:
			ret = 0;
			break;
		case 'y': case 'Y':
			if (m->buttons == 2)
				ret = 1;
			break;
		case 'n': case 'N':
			if (m->buttons == 2)
				ret = 0;
			break;
		case 'q': case 'o': case 'O':
			if (m->buttons == 1)
				ret = 1;
			break;
		}
	}
	ui_pop();
	return ret;
}

static void vmsg(const char *title, int tstyle, const char *fmt, va_list ap)
{
	char *text;
	struct msgdlg m = { .title = title, .tstyle = tstyle, .buttons = 1 };

	if (vasprintf(&text, fmt, ap) < 0)
		return;
	m.text = text;
	msg_run(&m);
	free(text);
}

void ui_msg(const char *title, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg(title, S_TITLE, fmt, ap);
	va_end(ap);
}

void ui_error(const char *title, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg(title, S_ERR, fmt, ap);
	va_end(ap);
}

int ui_yesno(const char *title, const char *text, int def_yes)
{
	struct msgdlg m = { .title = title, .tstyle = S_TITLE, .text = text,
			    .buttons = 2, .sel = def_yes ? 0 : 1 };

	return msg_run(&m);
}

static int yes_check(struct field *f, int n, char *note, size_t sz, void *ctx)
{
	(void)n;
	(void)ctx;
	if (!strcmp(f[0].buf, "yes")) {
		snprintf(note, sz, "Enter: go ahead");
		return 0;
	}
	snprintf(note, sz, "Type \"yes\" to go ahead, Esc to cancel.");
	return 1;
}

int ui_confirm_yes(const char *title, const char *text)
{
	struct field f[1] = { { .type = F_TEXT, .label = "Type \"yes\":",
				.maxlen = 3,
				.help = "Anything else leaves everything unchanged" } };

	return ui_form(title, text, f, 1, yes_check, NULL) &&
	       !strcmp(f[0].buf, "yes");
}

/* ---- list ---- */

struct listdlg {
	const char *title, *intro;
	const struct list_item *it;
	int n, sel, top, body;
};

static void list_draw(void *ctx)
{
	struct listdlg *l = ctx;
	int inner = imin(scr_cols - 6, 72), w = 0, h, y, x, i, nintro = 0;
	char **intro = NULL;

	if (l->intro)
		nintro = wrap(l->intro, inner, &intro);
	for (i = 0; i < nintro; i++)
		w = imax(w, strlen(intro[i]));
	for (i = 0; i < l->n; i++) {
		w = imax(w, strlen(l->it[i].text) + 2);
		if (l->it[i].help)
			w = imax(w, strlen(l->it[i].help));
	}
	w = imin(imax(w, (int)strlen(l->title) + 4), inner) + 4;
	l->body = imax(1, imin(l->n, scr_rows - 7 - nintro));
	if (l->sel < l->top)
		l->top = l->sel;
	if (l->sel >= l->top + l->body)
		l->top = l->sel - l->body + 1;
	h = l->body + 4 + (nintro ? nintro + 1 : 0);
	ui_frame(h, w, l->title, S_TITLE, &y, &x);
	for (i = 0; i < nintro; i++)
		scr_putn(y + 1 + i, x + 2, 0, intro[i], w - 4);
	y += nintro ? nintro + 1 : 0;
	for (i = 0; i < l->body && l->top + i < l->n; i++) {
		const struct list_item *it = &l->it[l->top + i];
		int s = l->top + i == l->sel ? S_SEL : it->disabled ? S_DIM : S_NORMAL;
		char line[256];

		snprintf(line, sizeof(line), " %s", it->text);
		scr_putn(y + 1 + i, x + 2, st(s), line, w - 4);
		if (it->hotkey && !it->disabled) {
			const char *p = strchr(it->text, it->hotkey);

			if (!p)
				p = strchr(it->text, it->hotkey ^ 0x20);
			if (p && p - it->text + 3 < w - 2)
				scr_fill(y + 1 + i, x + 3 + (int)(p - it->text),
					 1, st(l->top + i == l->sel ? S_HOT_SEL : S_HOT),
					 *p);
		}
	}
	if (l->n > l->body)
		scr_printf(y + l->body + 1, x + w - 12, st(S_DIM), " %d/%d ",
			   l->sel + 1, l->n);
	if (l->it[l->sel].help)
		scr_putn(y + l->body + 2, x + 2, st(S_DIM), l->it[l->sel].help, w - 4);
	free_lines(intro, nintro);
}

static int list_move(struct listdlg *l, int from, int dir)
{
	int i = from;

	for (;;) {
		i += dir;
		if (i < 0 || i >= l->n)
			return from;
		if (!l->it[i].disabled)
			return i;
	}
}

int ui_list(const char *title, const char *intro, const struct list_item *it,
	    int n, int sel)
{
	struct listdlg l = { title, intro, it, n, sel, 0, 1 };
	int k, ret = -2, i;

	if (n <= 0)
		return -1;
	if (l.sel < 0 || l.sel >= n || it[l.sel].disabled)
		l.sel = list_move(&l, -1, 1);
	if (l.sel < 0)
		return -1;
	ui_push(list_draw, &l);
	while (ret == -2) {
		ui_redraw();
		scr_flush();
		k = ui_getkey(-1);
		switch (k) {
		case KEY_UP: l.sel = list_move(&l, l.sel, -1); break;
		case KEY_DOWN: l.sel = list_move(&l, l.sel, 1); break;
		case KEY_HOME: l.sel = list_move(&l, -1, 1); break;
		case KEY_END: l.sel = list_move(&l, n, -1); break;
		case KEY_PGUP:
			for (i = 0; i < l.body; i++)
				l.sel = list_move(&l, l.sel, -1);
			break;
		case KEY_PGDN:
			for (i = 0; i < l.body; i++)
				l.sel = list_move(&l, l.sel, 1);
			break;
		case KEY_ENTER:
			if (!it[l.sel].disabled)
				ret = l.sel;
			break;
		case KEY_ESC: case 'q': case KEY_HANGUP:
			ret = -1;
			break;
		default:
			for (i = 0; i < n; i++) {
				if (it[i].hotkey && !it[i].disabled && k < 0x80 &&
				    (it[i].hotkey | 0x20) == (k | 0x20)) {
					ret = i;
					break;
				}
			}
		}
	}
	ui_pop();
	return ret;
}

/* ---- form ---- */

struct formdlg {
	const char *title, *intro;
	struct field *f;
	int n;
	form_check_fn check;
	void *ctx;
	int focus; /* index into visible fields, then OK, Cancel */
	char note[1024];
	int valid;
};

static int form_visible(struct formdlg *d, int *idx)
{
	int i, n = 0;

	for (i = 0; i < d->n; i++)
		if (!d->f[i].hidden)
			idx[n++] = i;
	return n;
}

static void form_draw(void *ctx)
{
	struct formdlg *d = ctx;
	int idx[32], nvis = form_visible(d, idx);
	int w = imin(scr_cols - 2, 78), inner = w - 4, lw = 0, fw, h, y, x, i;
	int nintro = 0, nnote = 0;
	char **intro = NULL, **note = NULL;
	const char *help = "Enter: OK   Esc: cancel   Tab/Up/Down: move";

	if (d->intro && *d->intro)
		nintro = wrap(d->intro, inner, &intro);
	if (*d->note)
		nnote = imin(wrap(d->note, inner, &note), 5);
	for (i = 0; i < nvis; i++)
		lw = imax(lw, strlen(d->f[idx[i]].label));
	lw = imin(lw, inner / 2);
	fw = inner - lw - 1;
	h = 2 + (nintro ? nintro + 1 : 0) + nvis + 1 + (nnote ? nnote + 1 : 0) + 2;
	if (h > scr_rows) {
		nintro = 0; /* drop the explanation on tiny terminals */
		h = 2 + nvis + 1 + (nnote ? nnote + 1 : 0) + 2;
	}
	ui_frame(h, w, d->title, S_TITLE, &y, &x);
	y++;
	for (i = 0; i < nintro; i++)
		scr_putn(y++, x + 2, 0, intro[i], inner);
	if (nintro)
		y++;
	for (i = 0; i < nvis; i++, y++) {
		struct field *f = &d->f[idx[i]];
		int focus = d->focus == i;
		int s = focus ? S_FIELD_FOCUS : S_FIELD;
		char buf[300];

		scr_putn(y, x + 2, focus ? st(S_TITLE) : 0, f->label, lw);
		switch (f->type) {
		case F_TEXT:
			if (f->pos < f->off)
				f->off = f->pos;
			if (f->pos >= f->off + fw)
				f->off = f->pos - fw + 1;
			scr_putn(y, x + 3 + lw, st(s), f->buf + imin(f->off, strlen(f->buf)),
				 fw);
			if (focus) {
				scr_cursor(y, x + 3 + lw + f->pos - f->off);
				if (f->browse)
					help = "F2 / Ctrl-B: browse files   Enter: OK   Esc: cancel";
			}
			break;
		case F_CHOICE:
			snprintf(buf, sizeof(buf), "< %s >", f->choices[f->choice]);
			scr_putn(y, x + 3 + lw, st(s), buf, imin(fw, strlen(buf)));
			if (focus)
				help = "Left/Right/Space: change   Enter: OK   Esc: cancel";
			break;
		case F_CHECK:
			scr_putn(y, x + 3 + lw, st(s), f->checked ? "[x]" : "[ ]", 3);
			if (focus)
				help = "Space: toggle   Enter: OK   Esc: cancel";
			break;
		}
		if (focus && f->help)
			help = f->help;
	}
	y++;
	for (i = 0; i < nnote; i++)
		scr_putn(y++, x + 2, st(d->valid ? S_DIM : S_ERR), note[i], inner);
	if (nnote)
		y++;
	{
		int bx = x + (w - 21) / 2;

		scr_put(y, bx, st(d->focus == nvis ? S_BUTTON_SEL : S_NORMAL),
			"[  OK  ]");
		scr_put(y, bx + 10, st(d->focus == nvis + 1 ? S_BUTTON_SEL : S_NORMAL),
			"[ Cancel ]");
	}
	scr_putn(y + 1, x + 2, st(S_DIM), help, inner);
	free_lines(intro, nintro);
	free_lines(note, nnote);
}

static void form_validate(struct formdlg *d)
{
	d->note[0] = 0;
	d->valid = d->check ? !d->check(d->f, d->n, d->note, sizeof(d->note), d->ctx)
			    : 1;
}

static int text_key(struct field *f, int k)
{
	int len = strlen(f->buf);

	switch (k) {
	case KEY_LEFT:
		if (f->pos > 0)
			f->pos--;
		return 0;
	case KEY_RIGHT:
		if (f->pos < len)
			f->pos++;
		return 0;
	case KEY_HOME: case KEY_CTRL('A'):
		f->pos = 0;
		return 0;
	case KEY_END: case KEY_CTRL('E'):
		f->pos = len;
		return 0;
	case KEY_BACKSPACE:
		if (f->pos > 0) {
			memmove(f->buf + f->pos - 1, f->buf + f->pos, len - f->pos + 1);
			f->pos--;
		}
		return 1;
	case KEY_DEL: case KEY_CTRL('D'):
		if (f->pos < len)
			memmove(f->buf + f->pos, f->buf + f->pos + 1, len - f->pos);
		return 1;
	case KEY_CTRL('U'):
		f->buf[0] = 0;
		f->pos = 0;
		return 1;
	case KEY_CTRL('K'):
		f->buf[f->pos] = 0;
		return 1;
	}
	if (k >= 0x20 && k < 0x7f) {
		int max = f->maxlen ? f->maxlen : (int)sizeof(f->buf) - 1;

		if (len >= max || len >= (int)sizeof(f->buf) - 1)
			return 0;
		memmove(f->buf + f->pos + 1, f->buf + f->pos, len - f->pos + 1);
		f->buf[f->pos++] = k;
		return 1;
	}
	return 0;
}

int ui_form(const char *title, const char *intro, struct field *f, int n,
	    form_check_fn check, void *ctx)
{
	struct formdlg d = { title, intro, f, n, check, ctx, 0, "", 1 };
	int idx[32], nvis, k, ret = -1, i;

	for (i = 0; i < n; i++)
		f[i].pos = strlen(f[i].buf);
	form_validate(&d);
	ui_push(form_draw, &d);
	while (ret < 0) {
		struct field *cf;

		nvis = form_visible(&d, idx);
		if (d.focus > nvis + 1)
			d.focus = nvis + 1;
		cf = d.focus < nvis ? &f[idx[d.focus]] : NULL;
		ui_redraw();
		scr_flush();
		k = ui_getkey(-1);
		switch (k) {
		case KEY_ESC: case KEY_HANGUP:
			ret = 0;
			continue;
		case KEY_TAB: case KEY_DOWN:
			d.focus = (d.focus + 1) % (nvis + 2);
			continue;
		case KEY_BTAB: case KEY_UP:
			d.focus = (d.focus + nvis + 1) % (nvis + 2);
			continue;
		case KEY_ENTER:
			if (d.focus == nvis + 1) {
				ret = 0;
				continue;
			}
			form_validate(&d);
			if (d.valid)
				ret = 1;
			continue;
		}
		if (!cf) {
			if (k == KEY_LEFT || k == KEY_RIGHT)
				d.focus = d.focus == nvis ? nvis + 1 : nvis;
			continue;
		}
		switch (cf->type) {
		case F_TEXT:
			if ((k == KEY_F2 || k == KEY_CTRL('B')) && cf->browse) {
				if (ui_browse(cf->buf, sizeof(cf->buf)))
					cf->pos = strlen(cf->buf);
				form_validate(&d);
			} else if (text_key(cf, k))
				form_validate(&d);
			break;
		case F_CHOICE:
			if (k == KEY_RIGHT || k == ' ') {
				cf->choice++;
				if (!cf->choices[cf->choice])
					cf->choice = 0;
				form_validate(&d);
			} else if (k == KEY_LEFT) {
				if (cf->choice == 0)
					while (cf->choices[cf->choice + 1])
						cf->choice++;
				else
					cf->choice--;
				form_validate(&d);
			}
			break;
		case F_CHECK:
			if (k == ' ' || k == KEY_LEFT || k == KEY_RIGHT) {
				cf->checked = !cf->checked;
				form_validate(&d);
			}
			break;
		}
	}
	ui_pop();
	return ret;
}

/* ---- documents ---- */

static void doc_put(struct doc *d, const char *s, size_t n)
{
	if (d->curlen + n + 1 > d->curcap) {
		d->curcap = (d->curlen + n + 1) * 2;
		d->cur = realloc(d->cur, d->curcap);
	}
	memcpy(d->cur + d->curlen, s, n);
	d->curlen += n;
	d->cur[d->curlen] = 0;
}

static void doc_endline(struct doc *d)
{
	if (d->n == d->cap) {
		d->cap = d->cap ? d->cap * 2 : 64;
		d->lines = realloc(d->lines, d->cap * sizeof(*d->lines));
	}
	d->lines[d->n++] = strdup(d->cur ? d->cur : "");
	d->curlen = 0;
	if (d->cur)
		d->cur[0] = 0;
}

void doc_style(struct doc *d, int style)
{
	char s[2] = { DOC_STYLE, (char)('A' + style) };

	doc_put(d, s, 2);
}

void doc_printf(struct doc *d, const char *fmt, ...)
{
	char *text, *p;
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vasprintf(&text, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	for (p = text; *p; p++) {
		if (*p == '\n') {
			doc_endline(d);
			continue;
		}
		doc_put(d, (unsigned char)*p < 0x20 ? "?" : p, 1);
	}
	free(text);
}

void doc_free(struct doc *d)
{
	free_lines(d->lines, d->n);
	free(d->cur);
	memset(d, 0, sizeof(*d));
}

struct viewdlg {
	const char *title;
	doc_build_fn build;
	void *ctx;
	struct doc doc;
	int built_width;
	int top, left, body;
};

static void view_draw(void *ctx)
{
	struct viewdlg *v = ctx;
	int w = scr_cols, h = scr_rows, y, x, i, inner = w - 4;

	if (v->built_width != inner) {
		doc_free(&v->doc);
		v->build(&v->doc, inner, v->ctx);
		if (v->doc.curlen)
			doc_endline(&v->doc);
		v->built_width = inner;
	}
	ui_frame(h, w, v->title, S_TITLE, &y, &x);
	v->body = imax(1, h - 2);
	if (v->top > v->doc.n - v->body)
		v->top = imax(0, v->doc.n - v->body);
	for (i = 0; i < v->body && v->top + i < v->doc.n; i++) {
		const char *p = v->doc.lines[v->top + i];
		uint16_t a = 0;
		int col = 0;

		for (; *p; p++) {
			if (*p == DOC_STYLE && p[1]) {
				a = st(p[1] - 'A');
				p++;
				continue;
			}
			if (col >= v->left && col - v->left < inner)
				scr_fill(y + 1 + i, x + 2 + col - v->left, 1, a, *p);
			col++;
		}
	}
	scr_printf(y + h - 1, x + 2, st(S_DIM),
		   " Up/Down/PgUp/PgDn/Left/Right: scroll   q: close ");
	if (v->doc.n > v->body)
		scr_printf(y + h - 1, x + w - 16, st(S_DIM), " %d-%d/%d ", v->top + 1,
			   imin(v->top + v->body, v->doc.n), v->doc.n);
}

void ui_view(const char *title, doc_build_fn build, void *ctx)
{
	struct viewdlg v = { .title = title, .build = build, .ctx = ctx,
			     .built_width = -1 };
	int k, done = 0;

	ui_push(view_draw, &v);
	while (!done) {
		ui_redraw();
		scr_flush();
		k = ui_getkey(-1);
		switch (k) {
		case KEY_UP: if (v.top > 0) v.top--; break;
		case KEY_DOWN: v.top++; break;
		case KEY_PGUP: v.top = imax(0, v.top - v.body); break;
		case KEY_PGDN: case ' ': v.top += v.body; break;
		case KEY_HOME: v.top = 0; break;
		case KEY_END: v.top = v.doc.n; break;
		case KEY_LEFT: v.left = imax(0, v.left - 8); break;
		case KEY_RIGHT: v.left += 8; break;
		case 'q': case 'Q': case KEY_ESC: case KEY_ENTER: case KEY_HANGUP:
			done = 1;
			break;
		}
	}
	ui_pop();
	doc_free(&v.doc);
}

/* ---- progress ---- */

static double elapsed(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) + (b->tv_nsec - a->tv_nsec) / 1e9;
}

static void prog_draw(void *ctx)
{
	struct progress *p = ctx;
	int w = imin(scr_cols - 4, 70), h = p->cancel_ok ? 8 : 7, y, x, bw, fill;
	char a[32], b[32], line[160];
	struct timespec now;
	double t;

	clock_gettime(CLOCK_MONOTONIC, &now);
	t = elapsed(&p->t0, &now);
	ui_frame(h, w, p->title, S_TITLE, &y, &x);
	scr_putn(y + 2, x + 2, 0, p->line, w - 4);
	bw = w - 4;
	fill = p->total > 0 ? (int)((double)p->done / p->total * bw) : 0;
	if (fill > bw)
		fill = bw;
	scr_fill(y + 3, x + 2, fill, term_mono ? A_REV : st(S_MAP1), ' ');
	scr_fill(y + 3, x + 2 + fill, bw - fill, 0, '.');
	if (p->total > 0) {
		char pct[8];

		snprintf(pct, sizeof(pct), "%3d%%", (int)(p->done * 100 / p->total));
		scr_put(y + 3, x + 2 + (bw - 4) / 2, A_BOLD, pct);
	}
	if (p->is_bytes) {
		snprintf(line, sizeof(line), "%s of %s", fmt_size(p->done, a, sizeof(a)),
			 fmt_size(p->total, b, sizeof(b)));
		if (t > 1 && p->done > 0) {
			double rate = p->done / t;
			int eta = p->total > p->done ? (int)((p->total - p->done) / rate) : 0;
			size_t n = strlen(line);

			snprintf(line + n, sizeof(line) - n, "   %s/s   ETA %d:%02d",
				 fmt_size((long long)rate, a, sizeof(a)), eta / 60, eta % 60);
		}
	} else {
		snprintf(line, sizeof(line), "%lld of %lld   %d:%02d elapsed", p->done,
			 p->total, (int)t / 60, (int)t % 60);
	}
	scr_putn(y + 4, x + 2, st(S_DIM), line, w - 4);
	if (p->cancel_ok)
		scr_putn(y + 6, x + 2, st(S_DIM), "Esc: cancel", w - 4);
}

void prog_start(struct progress *p, const char *title, long long total,
		int cancel_ok)
{
	memset(p, 0, sizeof(*p));
	p->title = title;
	p->total = total;
	p->cancel_ok = cancel_ok;
	p->is_bytes = 1;
	clock_gettime(CLOCK_MONOTONIC, &p->t0);
	ui_push(prog_draw, p);
	ui_redraw();
	scr_flush();
}

int prog_update(struct progress *p, long long done, const char *fmt, ...)
{
	struct timespec now;
	va_list ap;
	int k, force = done >= p->total;

	p->done = done;
	if (fmt) {
		va_start(ap, fmt);
		vsnprintf(p->line, sizeof(p->line), fmt, ap);
		va_end(ap);
	}
	while ((k = ui_getkey(0)) != KEY_NONE && k != KEY_HANGUP) {
		if (k == KEY_RESIZE)
			force = 1;
		if (p->cancel_ok && (k == KEY_ESC || k == 'q') &&
		    ui_yesno("Cancel", "Stop this operation?", 0))
			return 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (force || elapsed(&p->last, &now) > 0.15) {
		p->last = now;
		ui_redraw();
		scr_flush();
	}
	return 0;
}

void prog_end(struct progress *p)
{
	(void)p;
	ui_pop();
}

/* ---- file browser ---- */

struct entry {
	char *name;
	int dir;
	long long size;
};

struct browsedlg {
	char dir[512];
	struct entry *e;
	int n, sel, top, body;
};

static int entry_cmp(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;

	if (!strcmp(x->name, ".."))
		return -1;
	if (!strcmp(y->name, ".."))
		return 1;
	if (x->dir != y->dir)
		return y->dir - x->dir;
	return strcmp(x->name, y->name);
}

static void browse_load(struct browsedlg *b)
{
	DIR *d;
	struct dirent *de;
	int cap = 0;

	while (b->n > 0)
		free(b->e[--b->n].name);
	free(b->e);
	b->e = NULL;
	b->sel = b->top = 0;
	d = opendir(b->dir);
	if (!d)
		return;
	while ((de = readdir(d))) {
		char path[1024];
		struct stat s;

		if (!strcmp(de->d_name, ".") ||
		    (!strcmp(de->d_name, "..") && !strcmp(b->dir, "/")))
			continue;
		if (b->n == cap) {
			cap = cap ? cap * 2 : 64;
			b->e = realloc(b->e, cap * sizeof(*b->e));
		}
		snprintf(path, sizeof(path), "%s/%s", b->dir, de->d_name);
		b->e[b->n].name = strdup(de->d_name);
		b->e[b->n].dir = !stat(path, &s) && S_ISDIR(s.st_mode);
		b->e[b->n].size = b->e[b->n].dir ? 0 : s.st_size;
		b->n++;
	}
	closedir(d);
	qsort(b->e, b->n, sizeof(*b->e), entry_cmp);
}

static void browse_draw(void *ctx)
{
	struct browsedlg *b = ctx;
	int w = imin(scr_cols - 4, 72), h = scr_rows - 4, y, x, i;
	char title[600], sz[32];

	b->body = imax(1, h - 3);
	if (b->sel < b->top)
		b->top = b->sel;
	if (b->sel >= b->top + b->body)
		b->top = b->sel - b->body + 1;
	snprintf(title, sizeof(title), "%s", b->dir);
	ui_frame(h, w, title, S_TITLE, &y, &x);
	if (!b->n)
		scr_put(y + 1, x + 2, st(S_DIM), "(empty or unreadable)");
	for (i = 0; i < b->body && b->top + i < b->n; i++) {
		struct entry *e = &b->e[b->top + i];
		int s = b->top + i == b->sel ? S_SEL : e->dir ? S_TITLE : S_NORMAL;
		char line[600];

		if (e->dir)
			snprintf(line, sizeof(line), " %s/", e->name);
		else
			snprintf(line, sizeof(line), " %-*.*s %10s", w - 17, w - 17, e->name,
				 fmt_size(e->size, sz, sizeof(sz)));
		scr_putn(y + 1 + i, x + 2, st(s), line, w - 4);
	}
	scr_putn(y + h - 2, x + 2, st(S_DIM),
		 "Enter: open/choose   Backspace: up   Esc: cancel", w - 4);
}

static void browse_up(struct browsedlg *b)
{
	char *s = strrchr(b->dir, '/');

	if (s == b->dir)
		b->dir[1] = 0;
	else if (s)
		*s = 0;
	browse_load(b);
}

int ui_browse(char *path, size_t sz)
{
	struct browsedlg b = { .n = 0 };
	struct stat s;
	int k, ret = -1, i;

	if (path[0] == '/' && !stat(path, &s) && S_ISDIR(s.st_mode))
		snprintf(b.dir, sizeof(b.dir), "%s", path);
	else if (path[0] == '/' && strrchr(path, '/') != path) {
		snprintf(b.dir, sizeof(b.dir), "%s", path);
		*strrchr(b.dir, '/') = 0;
		if (stat(b.dir, &s) || !S_ISDIR(s.st_mode))
			b.dir[0] = 0;
	}
	if (!b.dir[0] && !getcwd(b.dir, sizeof(b.dir)))
		snprintf(b.dir, sizeof(b.dir), "/");
	if (strlen(b.dir) > 1 && b.dir[strlen(b.dir) - 1] == '/')
		b.dir[strlen(b.dir) - 1] = 0;
	browse_load(&b);
	ui_push(browse_draw, &b);
	while (ret < 0) {
		ui_redraw();
		scr_flush();
		k = ui_getkey(-1);
		switch (k) {
		case KEY_UP: if (b.sel > 0) b.sel--; break;
		case KEY_DOWN: if (b.sel < b.n - 1) b.sel++; break;
		case KEY_PGUP: b.sel = imax(0, b.sel - b.body); break;
		case KEY_PGDN: b.sel = imax(0, imin(b.n - 1, b.sel + b.body)); break;
		case KEY_HOME: b.sel = 0; break;
		case KEY_END: b.sel = imax(0, b.n - 1); break;
		case KEY_BACKSPACE: browse_up(&b); break;
		case KEY_ESC: case KEY_HANGUP: ret = 0; break;
		case KEY_ENTER:
			if (!b.n)
				break;
			if (!strcmp(b.e[b.sel].name, "..")) {
				browse_up(&b);
			} else if (b.e[b.sel].dir) {
				size_t l = strlen(b.dir);

				snprintf(b.dir + l, sizeof(b.dir) - l, "%s%s",
					 l > 1 ? "/" : "", b.e[b.sel].name);
				browse_load(&b);
			} else {
				snprintf(path, sz, "%s%s%s", b.dir, strlen(b.dir) > 1 ? "/" : "",
					 b.e[b.sel].name);
				ret = 1;
			}
			break;
		default:
			if (k > 0x20 && k < 0x7f) {
				for (i = 1; i <= b.n; i++) {
					int j = (b.sel + i) % b.n;

					if (b.e[j].name[0] == k) {
						b.sel = j;
						break;
					}
				}
			}
		}
	}
	ui_pop();
	while (b.n > 0)
		free(b.e[--b.n].name);
	free(b.e);
	return ret;
}

/* ---- formatting ---- */

char *fmt_size(long long bytes, char *buf, size_t sz)
{
	static const char *const unit[] = { "KiB", "MiB", "GiB" };
	long long div = 1024;
	int u = 0;

	if (bytes < 1024) {
		snprintf(buf, sz, "%lld B", bytes);
		return buf;
	}
	while (u < 2 && bytes >= div * 1024) {
		div *= 1024;
		u++;
	}
	if (bytes % div == 0)
		snprintf(buf, sz, "%lld %s", bytes / div, unit[u]);
	else
		snprintf(buf, sz, "%.1f %s", (double)bytes / div, unit[u]);
	return buf;
}

char *fmt_num(long long n, char *buf, size_t sz)
{
	char tmp[32];
	int len, i, j = 0, neg = n < 0;

	len = snprintf(tmp, sizeof(tmp), "%lld", neg ? -n : n);
	if (neg && j < (int)sz - 1)
		buf[j++] = '-';
	for (i = 0; i < len && j < (int)sz - 1; i++) {
		if (i && (len - i) % 3 == 0 && j < (int)sz - 1)
			buf[j++] = ',';
		buf[j++] = tmp[i];
	}
	buf[j] = 0;
	return buf;
}
