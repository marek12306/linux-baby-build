// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Terminal layer of ubitui: raw mode on the controlling terminal (/dev/tty),
 * a cell buffer flushed as a diff against the previous frame, and key input.
 *
 * Only VT100/ANSI sequences are used, so no terminfo database is needed (the
 * SqueezeOS root has none). Diff flushing keeps redraws small on the 115200
 * baud serial console. Over a serial line the kernel does not know the
 * terminal size, so it is probed with a cursor position report.
 */
#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define ESC_DELAY_MS 50

struct cell {
	unsigned char ch;
	uint16_t attr;
};

int scr_rows = 24, scr_cols = 80;
int term_mono;

static int tty = -1;
static struct termios saved;
static int raw_active;
static int ascii_lines;
static int size_probed;
static struct cell *cur, *old;
static int full_redraw = 1;
static int want_y = -1, want_x;
static int cursor_shown;
static volatile sig_atomic_t winch;
static volatile sig_atomic_t hangup;

static char *obuf;
static size_t olen, ocap;

static unsigned char ibuf[256];
static int ihead, itail;

static void oput(const char *s, size_t n)
{
	if (olen + n > ocap) {
		size_t cap = ocap ? ocap * 2 : 8192;

		while (cap < olen + n)
			cap *= 2;
		obuf = realloc(obuf, cap);
		if (!obuf)
			abort();
		ocap = cap;
	}
	memcpy(obuf + olen, s, n);
	olen += n;
}

static void oputs(const char *s)
{
	oput(s, strlen(s));
}

/* Write errors (a dropped SSH session) are ignored: the input side reports
 * the hangup, and a running flash operation must still finish. */
static void oflush(void)
{
	size_t off = 0;

	while (off < olen) {
		ssize_t n = write(tty, obuf + off, olen - off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		off += n;
	}
	olen = 0;
}

static const char restore_seq[] = "\033[0m\033(B\033[?7h\033[?25h\033[?1049l";

static void fatal_signal(int sig)
{
	if (raw_active) {
		if (write(tty, restore_seq, sizeof(restore_seq) - 1) < 0) {
			/* nothing left to do */
		}
		tcsetattr(tty, TCSAFLUSH, &saved);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static void on_winch(int sig)
{
	(void)sig;
	winch = 1;
}

static void on_hangup(int sig)
{
	(void)sig;
	hangup = 1;
}

static int rd(int timeout_ms)
{
	struct pollfd p = { .fd = tty, .events = POLLIN };
	ssize_t n;
	int r;

	if (ihead < itail)
		return ibuf[ihead++];
	ihead = itail = 0;
	r = poll(&p, 1, timeout_ms);
	if (r < 0)
		return errno == EINTR ? -2 : -3;
	if (r == 0)
		return -1;
	if (!(p.revents & POLLIN)) {
		hangup = 1;
		return -3;
	}
	n = read(tty, ibuf, sizeof(ibuf));
	if (n < 0 && errno == EINTR)
		return -2;
	if (n <= 0) {
		hangup = 1;
		return -3;
	}
	itail = n;
	return ibuf[ihead++];
}

/* Cursor position report probe for terminals whose size the kernel does not
 * know (serial console). Falls back to 24x80. */
static void probe_size(void)
{
	char resp[32];
	int len = 0, r, c, ch;

	oputs("\0337\033[999;999H\033[6n\0338");
	oflush();
	while (len < (int)sizeof(resp) - 1) {
		ch = rd(500);
		if (ch == -2)
			continue;
		if (ch < 0)
			break;
		resp[len++] = ch;
		if (ch == 'R')
			break;
	}
	resp[len] = 0;
	{
		char *e = strchr(resp, 27);

		if (e && sscanf(e, "\033[%d;%dR", &r, &c) == 2 && r > 0 && c > 0) {
			struct winsize ws = { .ws_row = r, .ws_col = c };

			scr_rows = r;
			scr_cols = c;
			/* Child processes and later queries see it too. */
			ioctl(tty, TIOCSWINSZ, &ws);
			size_probed = 1;
			return;
		}
	}
	scr_rows = 24;
	scr_cols = 80;
}

static void query_size(void)
{
	struct winsize ws;

	if (!size_probed && ioctl(tty, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
	    ws.ws_col > 0) {
		scr_rows = ws.ws_row;
		scr_cols = ws.ws_col;
		return;
	}
	probe_size();
}

static void alloc_cells(void)
{
	int i, n = scr_rows * scr_cols;

	free(cur);
	free(old);
	cur = malloc(n * sizeof(*cur));
	old = malloc(n * sizeof(*old));
	if (!cur || !old)
		abort();
	for (i = 0; i < n; i++) {
		cur[i] = (struct cell){ ' ', 0 };
		old[i] = (struct cell){ 0, 0xffff };
	}
	full_redraw = 1;
}

int term_open(int ascii, int mono)
{
	struct termios t;
	struct sigaction sa;

	ascii_lines = ascii;
	term_mono = mono;
	tty = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
	/* A serial console shell often has no controlling terminal; its stdin
	 * is the terminal, opened read-write. */
	if (tty < 0 && isatty(0) && (fcntl(0, F_GETFL) & O_ACCMODE) == O_RDWR)
		tty = fcntl(0, F_DUPFD_CLOEXEC, 3);
	if (tty < 0)
		return -1;
	if (tcgetattr(tty, &saved)) {
		close(tty);
		tty = -1;
		return -1;
	}
	t = saved;
	t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL |
		       IXON);
	t.c_oflag &= ~OPOST;
	t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	t.c_cflag &= ~(CSIZE | PARENB);
	t.c_cflag |= CS8;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(tty, TCSAFLUSH, &t)) {
		close(tty);
		tty = -1;
		return -1;
	}
	raw_active = 1;

	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = on_winch; /* no SA_RESTART: poll() must return */
	sigaction(SIGWINCH, &sa, NULL);
	sa.sa_handler = on_hangup;
	sigaction(SIGHUP, &sa, NULL);
	sa.sa_handler = fatal_signal;
	sa.sa_flags = SA_RESETHAND;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	oputs("\033[?1049h\033[?7l\033[?25l\033[0m\033(B\033[H\033[2J");
	oflush();
	query_size();
	alloc_cells();
	atexit(term_close);
	return 0;
}

void term_close(void)
{
	if (!raw_active)
		return;
	oputs(restore_seq);
	oflush();
	tcsetattr(tty, TCSAFLUSH, &saved);
	raw_active = 0;
}

void term_redraw(void)
{
	int r = scr_rows, c = scr_cols;

	if (size_probed)
		probe_size();
	else
		query_size();
	if (r != scr_rows || c != scr_cols)
		alloc_cells();
	full_redraw = 1;
}

static int parse_esc(void)
{
	int c = rd(ESC_DELAY_MS), f = 0, i, idx = 0;
	int par[4] = { 0 };

	if (c < 0)
		return KEY_ESC;
	if (c == 'O') {
		f = rd(ESC_DELAY_MS);
		switch (f) {
		case 'A': return KEY_UP;
		case 'B': return KEY_DOWN;
		case 'C': return KEY_RIGHT;
		case 'D': return KEY_LEFT;
		case 'H': return KEY_HOME;
		case 'F': return KEY_END;
		case 'P': return KEY_F1;
		case 'Q': return KEY_F2;
		case 'R': return KEY_F3;
		case 'S': return KEY_F4;
		}
		return KEY_NONE;
	}
	if (c != '[')
		return KEY_ESC; /* Alt+key: ignored */
	for (i = 0; i < 16; i++) {
		f = rd(ESC_DELAY_MS);
		if (f < 0)
			return KEY_NONE;
		if (i == 0 && f == '[') { /* Linux console F1-F5: ESC [ [ A..E */
			f = rd(ESC_DELAY_MS);
			return f >= 'A' && f <= 'E' ? KEY_F1 + f - 'A' : KEY_NONE;
		}
		if (f >= '0' && f <= '9')
			par[idx] = par[idx] * 10 + f - '0';
		else if (f == ';') {
			if (idx < 3)
				idx++;
		} else if (f >= 0x40 && f <= 0x7e)
			break;
	}
	switch (f) {
	case 'A': return KEY_UP;
	case 'B': return KEY_DOWN;
	case 'C': return KEY_RIGHT;
	case 'D': return KEY_LEFT;
	case 'H': return KEY_HOME;
	case 'F': return KEY_END;
	case 'Z': return KEY_BTAB;
	case 'P': return KEY_F1;
	case 'Q': return KEY_F2;
	case 'R': return KEY_F3;
	case 'S': return KEY_F4;
	case '~':
		switch (par[0]) {
		case 1: case 7: return KEY_HOME;
		case 2: return KEY_INS;
		case 3: return KEY_DEL;
		case 4: case 8: return KEY_END;
		case 5: return KEY_PGUP;
		case 6: return KEY_PGDN;
		case 11: case 12: case 13: case 14: case 15:
			return KEY_F1 + par[0] - 11;
		case 17: case 18: case 19: case 20: case 21:
			return KEY_F6 + par[0] - 17;
		}
	}
	return KEY_NONE;
}

int term_getkey(int timeout_ms)
{
	for (;;) {
		int c;

		if (winch) {
			winch = 0;
			term_redraw();
			return KEY_RESIZE;
		}
		if (hangup)
			return KEY_HANGUP;
		c = rd(timeout_ms);
		if (c == -2)
			continue;
		if (c == -3)
			return KEY_HANGUP;
		if (c < 0)
			return KEY_NONE;
		if (c == 27) {
			c = parse_esc();
			if (c == KEY_NONE && timeout_ms != 0)
				continue;
			return c;
		}
		if (c == '\r' || c == '\n')
			return KEY_ENTER;
		if (c == 127 || c == 8)
			return KEY_BACKSPACE;
		return c;
	}
}

void scr_clear(void)
{
	int i, n = scr_rows * scr_cols;

	for (i = 0; i < n; i++)
		cur[i] = (struct cell){ ' ', 0 };
	want_y = -1;
}

static void setc(int y, int x, uint16_t attr, int ch)
{
	if (y < 0 || y >= scr_rows || x < 0 || x >= scr_cols)
		return;
	if (!(attr & A_ACS) && (ch < 0x20 || ch > 0x7e))
		ch = '?';
	cur[y * scr_cols + x] = (struct cell){ (unsigned char)ch, attr };
}

void scr_fill(int y, int x, int n, uint16_t attr, int ch)
{
	while (n-- > 0)
		setc(y, x++, attr, ch);
}

void scr_putn(int y, int x, uint16_t attr, const char *s, int n)
{
	int i;

	for (i = 0; s[i] && (n < 0 || i < n); i++)
		setc(y, x + i, attr, (unsigned char)s[i]);
	for (; i < n; i++)
		setc(y, x + i, attr, ' ');
}

void scr_put(int y, int x, uint16_t attr, const char *s)
{
	scr_putn(y, x, attr, s, -1);
}

void scr_printf(int y, int x, uint16_t attr, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	scr_put(y, x, attr, buf);
}

void scr_acs(int y, int x, uint16_t attr, int acs)
{
	setc(y, x, attr | A_ACS, acs);
}

void scr_hline(int y, int x, int n, uint16_t attr)
{
	while (n-- > 0)
		scr_acs(y, x++, attr, ACS_HLINE);
}

void scr_box(int y, int x, int h, int w, uint16_t attr)
{
	int i;

	scr_acs(y, x, attr, ACS_ULCORNER);
	scr_hline(y, x + 1, w - 2, attr);
	scr_acs(y, x + w - 1, attr, ACS_URCORNER);
	for (i = 1; i < h - 1; i++) {
		scr_acs(y + i, x, attr, ACS_VLINE);
		scr_acs(y + i, x + w - 1, attr, ACS_VLINE);
	}
	scr_acs(y + h - 1, x, attr, ACS_LLCORNER);
	scr_hline(y + h - 1, x + 1, w - 2, attr);
	scr_acs(y + h - 1, x + w - 1, attr, ACS_LRCORNER);
}

void scr_cursor(int y, int x)
{
	want_y = y;
	want_x = x;
}

static int acs_ascii(int ch)
{
	switch (ch) {
	case ACS_HLINE: return '-';
	case ACS_VLINE: return '|';
	case ACS_CKBOARD: return ':';
	default: return '+';
	}
}

static void sgr(uint16_t a)
{
	char b[40];
	int n = snprintf(b, sizeof(b), "\033[0");

	if (a & A_BOLD)
		n += snprintf(b + n, sizeof(b) - n, ";1");
	if (a & A_UNDER)
		n += snprintf(b + n, sizeof(b) - n, ";4");
	if (a & A_REV)
		n += snprintf(b + n, sizeof(b) - n, ";7");
	if (!term_mono && (a & 0xf))
		n += snprintf(b + n, sizeof(b) - n, ";3%d", (a & 0xf) - 1);
	if (!term_mono && (a & 0xf0))
		n += snprintf(b + n, sizeof(b) - n, ";4%d", ((a >> 4) & 0xf) - 1);
	snprintf(b + n, sizeof(b) - n, "m");
	oputs(b);
}

void scr_flush(void)
{
	char b[32];
	int y, x, cy = -1, cx = -1, acs_on = 0;
	uint16_t ca = 0xffff;

	if (full_redraw) {
		oputs("\033[0m\033(B\033[2J");
		for (x = 0; x < scr_rows * scr_cols; x++)
			old[x] = (struct cell){ 0, 0xffff };
		full_redraw = 0;
	}
	for (y = 0; y < scr_rows; y++) {
		for (x = 0; x < scr_cols; x++) {
			struct cell *c = &cur[y * scr_cols + x];
			struct cell *o = &old[y * scr_cols + x];
			uint16_t a = c->attr & ~A_ACS;
			int acs = (c->attr & A_ACS) && !ascii_lines;
			int ch = c->ch;

			/* The bottom-right cell is never written: terminals that
			 * ignore the autowrap reset would scroll. */
			if ((c->ch == o->ch && c->attr == o->attr) ||
			    (y == scr_rows - 1 && x == scr_cols - 1))
				continue;
			if (cy != y || cx != x) {
				snprintf(b, sizeof(b), "\033[%d;%dH", y + 1, x + 1);
				oputs(b);
			}
			if (a != ca) {
				sgr(a);
				ca = a;
			}
			if (acs != acs_on) {
				oputs(acs ? "\033(0" : "\033(B");
				acs_on = acs;
			}
			if ((c->attr & A_ACS) && ascii_lines)
				ch = acs_ascii(ch);
			b[0] = ch;
			oput(b, 1);
			*o = *c;
			cy = y;
			cx = x + 1;
		}
	}
	if (acs_on)
		oputs("\033(B");
	if (ca != 0xffff && ca != 0)
		oputs("\033[0m");
	if (want_y >= 0 && want_y < scr_rows && want_x < scr_cols) {
		snprintf(b, sizeof(b), "\033[%d;%dH", want_y + 1, want_x + 1);
		oputs(b);
		if (!cursor_shown)
			oputs("\033[?25h");
		cursor_shown = 1;
	} else if (cursor_shown) {
		oputs("\033[?25l");
		cursor_shown = 0;
	}
	oflush();
}
