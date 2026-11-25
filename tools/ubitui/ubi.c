// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Backend of ubitui: the device/volume model read through libubi and sysfs,
 * size parsing, error collection (library messages and kernel log) and the
 * input streams for volume updates and formatting.
 */
#include "ubi.h"
#include "ui.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/klog.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

libubi_t lubi;
libmtd_t lmtd;

void backend_open(void)
{
	lubi = libubi_open();
	lmtd = libmtd_open();
}

void dev_node(int num, char *buf, size_t sz)
{
	snprintf(buf, sz, "/dev/ubi%d", num);
}

void vol_node(int dev, int vol, char *buf, size_t sz)
{
	snprintf(buf, sz, "/dev/ubi%d_%d", dev, vol);
}

int sysfs_int(const char *fmt, int a, int b, int def)
{
	char path[160], buf[32];
	FILE *f;

	snprintf(path, sizeof(path), fmt, a, b);
	f = fopen(path, "r");
	if (!f)
		return def;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = 0;
	fclose(f);
	return buf[0] ? atoi(buf) : def;
}

int vol_find_name(const struct dev *d, const char *name)
{
	int i;

	for (i = 0; i < d->nvols; i++)
		if (!strcmp(d->vols[i].info.name, name))
			return i;
	return -1;
}

static int count_mapped(int dev, int vol, int lebs)
{
	char node[64];
	int fd, l, n = 0, r;

	vol_node(dev, vol, node, sizeof(node));
	fd = open(node, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	for (l = 0; l < lebs; l++) {
		r = ubi_is_mapped(fd, l);
		if (r < 0) {
			n = -1;
			break;
		}
		n += r;
	}
	close(fd);
	return n;
}

static void add_use(struct vol *v, const char *fmt, ...)
{
	size_t n = strlen(v->use);
	va_list ap;

	if (n && n < sizeof(v->use) - 2) {
		strcpy(v->use + n, ", ");
		n += 2;
	}
	va_start(ap, fmt);
	vsnprintf(v->use + n, sizeof(v->use) - n, fmt, ap);
	va_end(ap);
}

/* Mounts naming the volume as UBIFS (ubiX_Y, ubiX:NAME, ubi:NAME for ubi0)
 * or through its ubiblock device. */
static void find_uses(struct dev *d)
{
	char line[1024], src[512], mnt[512], type[64], a[64];
	FILE *f = fopen("/proc/mounts", "r");
	int i;

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%511s %511s %63s", src, mnt, type) != 3)
			continue;
		for (i = 0; i < d->nvols; i++) {
			struct vol *v = &d->vols[i];
			const char *s = src;

			if (!strncmp(s, "/dev/", 5))
				s += 5;
			snprintf(a, sizeof(a), "ubiblock%d_%d", d->num, v->info.vol_id);
			if (!strcmp(s, a)) {
				add_use(v, "%s mounted on %s (%s)", a, mnt, type);
				continue;
			}
			snprintf(a, sizeof(a), "ubi%d_%d", d->num, v->info.vol_id);
			if (!strcmp(s, a)) {
				add_use(v, "mounted on %s (%s)", mnt, type);
				continue;
			}
			snprintf(a, sizeof(a), "ubi%d:", d->num);
			if ((!strncmp(s, a, strlen(a)) &&
			     !strcmp(s + strlen(a), v->info.name)) ||
			    (d->num == 0 && !strncmp(s, "ubi:", 4) &&
			     !strcmp(s + 4, v->info.name)))
				add_use(v, "mounted on %s (%s)", mnt, type);
		}
	}
	fclose(f);
	for (i = 0; i < d->nvols; i++) {
		struct vol *v = &d->vols[i];

		snprintf(a, sizeof(a), "ubiblock%d_%d", d->num, v->info.vol_id);
		if (v->block && !strstr(v->use, a))
			add_use(v, "%s", a);
	}
}

int dev_load(struct dev *d, int num)
{
	char path[96];
	int id;

	memset(d, 0, sizeof(*d));
	d->num = num;
	if (!lubi || ubi_get_dev_info1(lubi, num, &d->info))
		return -1;
	if (lmtd && !mtd_get_dev_info1(lmtd, d->info.mtd_num, &d->mtd))
		d->have_mtd = 1;
	d->ro_mode = sysfs_int("/sys/class/ubi/ubi%d/ro_mode", num, 0, 0);
	for (id = d->info.lowest_vol_id;
	     id <= d->info.highest_vol_id && d->nvols < MAX_VOLS; id++) {
		struct vol *v = &d->vols[d->nvols];

		if (ubi_get_vol_info1(lubi, num, id, &v->info))
			continue;
		v->upd_marker = sysfs_int("/sys/class/ubi/ubi%d_%d/upd_marker", num,
					  id, 0);
		v->mapped = count_mapped(num, id, v->info.rsvd_lebs);
		snprintf(path, sizeof(path), "/sys/block/ubiblock%d_%d", num, id);
		v->block = !access(path, F_OK);
		d->nvols++;
	}
	find_uses(d);
	return 0;
}

long long parse_size(const char *s, long long leb, long long max_bytes,
		     long long *lebs, char *err, size_t esz)
{
	static const struct {
		const char *unit;
		long long mult;
	} units[] = {
		{ "", 1 },	    { "b", 1 },		  { "k", 1024 },
		{ "kib", 1024 },    { "kb", 1000 },	  { "m", 1 << 20 },
		{ "mib", 1 << 20 }, { "mb", 1000000 },	  { "g", 1 << 30 },
		{ "gib", 1 << 30 }, { "gb", 1000000000 },
	};
	char *end, unit[8];
	double v;
	size_t i, n = 0;

	*lebs = -1;
	while (isspace((unsigned char)*s))
		s++;
	if (!*s) {
		snprintf(err, esz, "Enter a size, e.g. 12MiB, 96LEB or max.");
		return -1;
	}
	if (!strncasecmp(s, "max", 3) || !strncasecmp(s, "all", 3)) {
		if (max_bytes <= 0) {
			snprintf(err, esz, "No free space.");
			return -1;
		}
		return max_bytes;
	}
	errno = 0;
	v = strtod(s, &end);
	if (end == s || errno || v < 0) {
		snprintf(err, esz, "Not a number: \"%s\".", s);
		return -1;
	}
	while (isspace((unsigned char)*end))
		end++;
	while (*end && !isspace((unsigned char)*end) && n < sizeof(unit) - 1)
		unit[n++] = tolower((unsigned char)*end++);
	unit[n] = 0;
	while (isspace((unsigned char)*end))
		end++;
	if (*end) {
		snprintf(err, esz, "Unexpected \"%s\".", end);
		return -1;
	}
	if (!strcmp(unit, "l") || !strcmp(unit, "leb") || !strcmp(unit, "lebs")) {
		if (v != (long long)v) {
			snprintf(err, esz, "A LEB count must be a whole number.");
			return -1;
		}
		*lebs = (long long)v;
		return *lebs * leb;
	}
	for (i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
		if (!strcmp(unit, units[i].unit)) {
			if (units[i].mult == 1 && v != (long long)v) {
				snprintf(err, esz, "Bytes must be a whole number.");
				return -1;
			}
			return (long long)(v * units[i].mult + 0.5);
		}
	}
	snprintf(err, esz,
		 "Unknown unit \"%s\" (use B, KiB, MiB, GiB, KB, MB, GB or LEB).",
		 unit);
	return -1;
}

/* ---- error collection ---- */

static int errpipe[2] = { -1, -1 };
static int saved_stderr = -1;
static char klog_mark[512];

static void errcap_restore(void)
{
	char buf[4096];
	ssize_t n;

	if (saved_stderr < 0)
		return;
	dup2(saved_stderr, 2);
	while ((n = read(errpipe[0], buf, sizeof(buf))) > 0)
		if (write(2, buf, n) < 0)
			break;
}

/* libubi/libmtd report details on stderr, which would scribble over the
 * screen; a pipe collects them instead. */
void errcap_init(void)
{
	if (pipe(errpipe))
		return;
	fcntl(errpipe[0], F_SETFL, O_NONBLOCK);
	fcntl(errpipe[1], F_SETFL, O_NONBLOCK);
	fcntl(errpipe[0], F_SETFD, FD_CLOEXEC);
	fcntl(errpipe[1], F_SETFD, FD_CLOEXEC);
	saved_stderr = fcntl(2, F_DUPFD_CLOEXEC, 3);
	dup2(errpipe[1], 2);
	atexit(errcap_restore);
}

static void errcap_take(char *out, size_t sz)
{
	char rest[1024];
	size_t n = strlen(out);
	ssize_t r;

	if (errpipe[0] < 0)
		return;
	fflush(stderr);
	while (n < sz - 1 && (r = read(errpipe[0], out + n, sz - 1 - n)) > 0)
		n += r;
	out[n] = 0;
	while (read(errpipe[0], rest, sizeof(rest)) > 0)
		; /* drop what does not fit */
}

static char *klog_read(void)
{
	int size = klogctl(10, NULL, 0), n;
	char *buf;

	if (size <= 0)
		size = 1 << 17;
	buf = malloc(size + 1);
	if (!buf)
		return NULL;
	n = klogctl(3, buf, size);
	if (n < 0) {
		free(buf);
		return NULL;
	}
	buf[n] = 0;
	return buf;
}

static void klog_set_mark(void)
{
	char *buf = klog_read(), *end, *start;

	klog_mark[0] = 0;
	if (!buf)
		return;
	end = buf + strlen(buf);
	while (end > buf && end[-1] == '\n')
		end--;
	*end = 0;
	start = strrchr(buf, '\n');
	start = start ? start + 1 : buf;
	snprintf(klog_mark, sizeof(klog_mark), "%s", start);
	free(buf);
}

/* UBI/MTD kernel messages logged since op_begin(). */
static void klog_new(char *out, size_t sz)
{
	char *buf = klog_read(), *p, *line, *save;
	size_t n = strlen(out);

	if (!buf)
		return;
	p = buf;
	if (klog_mark[0]) {
		char *q = buf, *last = NULL;

		while ((q = strstr(q, klog_mark))) {
			last = q;
			q++;
		}
		if (last)
			p = last + strlen(klog_mark);
	}
	for (line = strtok_r(p, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *t = line;

		if (*t == '<') { /* "<3>" level prefix */
			char *gt = strchr(t, '>');

			if (gt)
				t = gt + 1;
		}
		if (!strcasestr(t, "ubi") && !strcasestr(t, "mtd") &&
		    !strcasestr(t, "nand"))
			continue;
		n += snprintf(out + n, n < sz ? sz - n : 0, "  %s\n", t);
		if (n >= sz)
			break;
	}
	free(buf);
}

void op_begin(void)
{
	char junk[4096] = "";

	errcap_take(junk, sizeof(junk));
	klog_set_mark();
}

static const char *errno_hint(int err)
{
	switch (err) {
	case EBUSY:
		return "It is in use: mounted, opened by a program, exported as a "
		       "ubiblock device, or (for MTD) attached to UBI.";
	case ENOSPC:
		return "Not enough free eraseblocks.";
	case EEXIST:
		return "A volume with this name or ID already exists.";
	case EROFS:
		return "The device is read-only (UBI switches to read-only mode "
		       "after serious errors).";
	case EPERM:
		return "Not permitted (a static volume cannot shrink below its "
		       "data; some operations need root).";
	case EACCES:
		return "Permission denied: run as root.";
	case ENOTTY:
		return "The running kernel does not support this operation.";
	case ENODEV:
		return "The device or volume no longer exists.";
	case EINVAL:
		return "The kernel rejected a parameter; see its log below.";
	case ENAMETOOLONG:
		return "The name is too long (at most 127 characters).";
	}
	return NULL;
}

void op_fail(const char *title, int err, const char *fmt, ...)
{
	char text[8192], lib[4096] = "", kern[4096] = "";
	const char *hint = errno_hint(err);
	va_list ap;
	size_t n;

	va_start(ap, fmt);
	n = vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	if (err && n < sizeof(text))
		n += snprintf(text + n, sizeof(text) - n, "\n\nError %d: %s", err,
			      strerror(err));
	if (hint && n < sizeof(text))
		n += snprintf(text + n, sizeof(text) - n, "\n%s", hint);
	errcap_take(lib, sizeof(lib));
	klog_new(kern, sizeof(kern));
	if (*lib && n < sizeof(text))
		n += snprintf(text + n, sizeof(text) - n, "\n\nLibrary messages:\n%s",
			      lib);
	if (*kern && n < sizeof(text))
		snprintf(text + n, sizeof(text) - n, "\n\nKernel log:\n%s", kern);
	ui_error(title, "%s", text);
}

/* ---- input streams ---- */

static int http_open(struct src *s, const char *url, char *err, size_t esz)
{
	char host[256], port[16] = "80", req[1200], hdr[8192];
	const char *p = url + 7, *path = strchr(p, '/'), *colon;
	struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *ai, *a;
	struct timeval tv = { .tv_sec = 30 };
	int fd = -1, len = 0, status, r;
	size_t hl = path ? (size_t)(path - p) : strlen(p);
	char *eoh, *line;

	_Static_assert(sizeof(s->pre) >= sizeof(hdr), "pre holds a header read");

	if (!path)
		path = "/";
	if (hl >= sizeof(host)) {
		snprintf(err, esz, "Host name too long.");
		return -1;
	}
	memcpy(host, p, hl);
	host[hl] = 0;
	colon = strrchr(host, ':');
	if (colon && !strchr(host, ']')) {
		snprintf(port, sizeof(port), "%s", colon + 1);
		host[colon - host] = 0;
	}
	r = getaddrinfo(host, port, &hints, &ai);
	if (r) {
		snprintf(err, esz, "%s: %s", host, gai_strerror(r));
		return -1;
	}
	for (a = ai; a; a = a->ai_next) {
		fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		if (!connect(fd, a->ai_addr, a->ai_addrlen))
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(ai);
	if (fd < 0) {
		snprintf(err, esz, "Cannot connect to %s:%s: %s", host, port,
			 strerror(errno));
		return -1;
	}
	r = snprintf(req, sizeof(req),
		     "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ubitui\r\n"
		     "Connection: close\r\n\r\n", path, host);
	if (write(fd, req, r) != r) {
		snprintf(err, esz, "Cannot send the request: %s", strerror(errno));
		close(fd);
		return -1;
	}
	for (;;) {
		ssize_t n = read(fd, hdr + len, sizeof(hdr) - 1 - len);

		if (n <= 0) {
			snprintf(err, esz, "No HTTP response: %s",
				 n ? strerror(errno) : "connection closed");
			close(fd);
			return -1;
		}
		len += n;
		hdr[len] = 0;
		eoh = strstr(hdr, "\r\n\r\n");
		if (eoh)
			break;
		if (len == sizeof(hdr) - 1) {
			snprintf(err, esz, "HTTP headers too long.");
			close(fd);
			return -1;
		}
	}
	*eoh = 0;
	if (sscanf(hdr, "HTTP/%*s %d", &status) != 1 || status != 200) {
		line = strstr(hdr, "\r\n");
		if (line)
			*line = 0;
		snprintf(err, esz, "Server answered \"%s\".", hdr);
		close(fd);
		return -1;
	}
	s->size = -1;
	for (line = strstr(hdr, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
		if (!strncasecmp(line + 2, "Content-Length:", 15))
			s->size = strtoll(line + 17, NULL, 10);
	}
	s->prelen = len - (int)(eoh + 4 - hdr);
	memcpy(s->pre, eoh + 4, s->prelen);
	s->fd = fd;
	return 0;
}

int src_open(struct src *s, const char *spec, long long skip, char *err,
	     size_t esz)
{
	char buf[65536];
	long long left = skip;

	memset(s, 0, sizeof(*s));
	s->fd = -1;
	s->size = -1;
	if (!strcmp(spec, "-")) {
		if (isatty(0)) {
			snprintf(err, esz, "Standard input is the terminal. Start "
				 "ubitui with data piped in to use \"-\".");
			return -1;
		}
		s->fd = 0;
	} else if (!strncasecmp(spec, "http://", 7)) {
		if (http_open(s, spec, err, esz))
			return -1;
	} else {
		struct stat st;
		off_t end;

		s->fd = open(spec, O_RDONLY | O_CLOEXEC);
		if (s->fd < 0 || fstat(s->fd, &st)) {
			snprintf(err, esz, "%s: %s", spec, strerror(errno));
			if (s->fd >= 0)
				close(s->fd);
			s->fd = -1;
			return -1;
		}
		if (S_ISREG(st.st_mode)) {
			s->size = st.st_size;
		} else if ((end = lseek(s->fd, 0, SEEK_END)) > 0) {
			s->size = end; /* devices such as /dev/ubi0_3 */
			lseek(s->fd, 0, SEEK_SET);
		}
		if (skip && s->size >= 0 && lseek(s->fd, skip, SEEK_SET) == skip)
			left = 0;
	}
	while (left > 0) {
		long long n = src_read(s, buf, left < (long long)sizeof(buf) ?
					       left : (long long)sizeof(buf));

		if (n <= 0) {
			snprintf(err, esz, "The input ends before %lld bytes.", skip);
			src_close(s);
			return -1;
		}
		left -= n;
	}
	if (skip && s->size >= 0)
		s->size = s->size > skip ? s->size - skip : 0;
	return 0;
}

long long src_read(struct src *s, void *buf, long long n)
{
	long long got = 0;

	if (s->preoff < s->prelen) {
		long long take = s->prelen - s->preoff;

		if (take > n)
			take = n;
		memcpy(buf, s->pre + s->preoff, take);
		s->preoff += take;
		got = take;
	}
	while (got < n) {
		ssize_t r = read(s->fd, (char *)buf + got, n - got);

		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			return got ? got : -1;
		if (r == 0)
			break;
		got += r;
	}
	return got;
}

void src_close(struct src *s)
{
	if (s->fd > 0)
		close(s->fd);
	s->fd = -1;
}
