// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Raw flash operations of ubitui: scanning UBI headers on an MTD device
 * (ubiscan), formatting it (ubiformat), the live erase counter map of an
 * attached device (UBI_IOCECNFO) and a read check of every PEB that lets UBI
 * scrub bit-flips (UBI_IOCRPEB, as ubihealthd does).
 *
 * The scan and format code follows mtd-utils 2.2.1 ubi-utils/ubiformat.c and
 * lib/libscan.c (Artem Bityutskiy, Nokia, GPL-2.0-or-later); the prompts and
 * progress output are replaced by dialogs.
 */
#include "flash.h"
#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <crc32.h>
#include <mtd/ubi-media.h> /* before libubigen.h, which uses its structs */
#include <libubigen.h>
#include <mtd_swab.h>

#ifndef UBI_IOCRPEB
#define UBI_IOCRPEB _IOW(UBI_IOC_MAGIC, 4, int32_t)
#endif
#ifndef UBI_IOCECNFO
struct ubi_ecinfo_req {
	int32_t start;
	int32_t length;
	int32_t read_length;
	int8_t padding[16];
	int32_t erase_counters[];
} __attribute__((packed));
#define UBI_IOCECNFO _IOWR(UBI_IOC_MAGIC, 6, struct ubi_ecinfo_req)
#endif

#define MAX_CONSECUTIVE_BAD_BLOCKS 4

/* Per-PEB values below zero */
enum { PEB_UNKNOWN = -1, PEB_BAD = -2, PEB_EMPTY = -3, PEB_CORRUPT = -4,
       PEB_ALIEN = -5 };

struct scan {
	long long *ec; /* EC or PEB_* */
	int eb_cnt;
	int ok, empty, corrupted, alien, bad, good;
	long long mean;
	int vid_hdr_offs, data_offs;
};

static int all_ff(const void *buf, int len)
{
	const unsigned char *p = buf;
	int i;

	for (i = 0; i < len; i++)
		if (p[i] != 0xff)
			return 0;
	return 1;
}

static int open_mtd(int mtd_num, int flags, struct mtd_dev_info *mtd)
{
	char node[32];

	if (!lmtd || mtd_get_dev_info1(lmtd, mtd_num, mtd))
		return -1;
	snprintf(node, sizeof(node), "/dev/mtd%d", mtd_num);
	return open(node, flags | O_CLOEXEC);
}

/* Returns 0, -1 with errno set, or 1 when cancelled. */
static int mtd_scan(const struct mtd_dev_info *mtd, int fd, struct scan *s)
{
	struct progress p;
	unsigned long long sum = 0;
	int eb, ret = 0;

	memset(s, 0, sizeof(*s));
	s->eb_cnt = mtd->eb_cnt;
	s->ec = calloc(mtd->eb_cnt, sizeof(*s->ec));
	if (!s->ec)
		return -1;
	s->vid_hdr_offs = s->data_offs = -1;
	prog_start(&p, "Scanning", mtd->eb_cnt, 1);
	p.is_bytes = 0;
	for (eb = 0; eb < mtd->eb_cnt; eb++) {
		struct ubi_ec_hdr ech;
		unsigned long long ec;
		int r;

		if (prog_update(&p, eb, "Reading UBI headers of mtd%d", mtd->mtd_num)) {
			ret = 1;
			break;
		}
		r = mtd_is_bad(mtd, fd, eb);
		if (r < 0) {
			ret = -1;
			break;
		}
		if (r) {
			s->bad++;
			s->ec[eb] = PEB_BAD;
			continue;
		}
		if (mtd_read(mtd, fd, eb, 0, &ech, sizeof(ech))) {
			ret = -1;
			break;
		}
		if (be32_to_cpu(ech.magic) != UBI_EC_HDR_MAGIC) {
			if (all_ff(&ech, sizeof(ech))) {
				s->empty++;
				s->ec[eb] = PEB_EMPTY;
			} else {
				s->alien++;
				s->ec[eb] = PEB_ALIEN;
			}
			continue;
		}
		if (be32_to_cpu(ech.hdr_crc) !=
		    mtd_crc32(UBI_CRC32_INIT, &ech, UBI_EC_HDR_SIZE_CRC)) {
			s->corrupted++;
			s->ec[eb] = PEB_CORRUPT;
			continue;
		}
		ec = be64_to_cpu(ech.ec);
		if (ec > UBI_MAX_ERASECOUNTER) {
			s->corrupted++;
			s->ec[eb] = PEB_CORRUPT;
			continue;
		}
		if (s->vid_hdr_offs == -1) {
			if ((int)be32_to_cpu(ech.data_offset) % mtd->min_io_size) {
				s->corrupted++;
				s->ec[eb] = PEB_CORRUPT;
				continue;
			}
			s->vid_hdr_offs = be32_to_cpu(ech.vid_hdr_offset);
			s->data_offs = be32_to_cpu(ech.data_offset);
		} else if ((int)be32_to_cpu(ech.vid_hdr_offset) != s->vid_hdr_offs ||
			   (int)be32_to_cpu(ech.data_offset) != s->data_offs) {
			s->corrupted++;
			s->ec[eb] = PEB_CORRUPT;
			continue;
		}
		s->ok++;
		s->ec[eb] = ec;
		sum += ec;
	}
	prog_end(&p);
	if (ret) {
		int e = errno;

		free(s->ec);
		s->ec = NULL;
		errno = e;
		return ret;
	}
	if (s->ok)
		s->mean = sum / s->ok;
	s->good = mtd->eb_cnt - s->bad;
	return 0;
}

/* ---- erase counter documents ---- */

static const int ec_styles[5] = { S_MAP2, S_MAP4, S_MAP5, S_MAP3, S_MAP6 };

static void ec_range(const long long *ec, int n, long long *min, long long *max,
		     long long *mean, int *valid)
{
	long long sum = 0;
	int i;

	*min = -1;
	*max = -1;
	*valid = 0;
	for (i = 0; i < n; i++) {
		if (ec[i] < 0)
			continue;
		if (*min < 0 || ec[i] < *min)
			*min = ec[i];
		if (ec[i] > *max)
			*max = ec[i];
		sum += ec[i];
		(*valid)++;
	}
	*mean = *valid ? sum / *valid : 0;
}

static int ec_bucket(long long v, long long min, long long max)
{
	if (max <= min)
		return 0;
	return (int)((v - min) * 5 / (max - min + 1));
}

static void ec_doc(struct doc *d, int width, const long long *ec, int n)
{
	long long min, max, mean;
	int valid, i, j, per, cnt[5] = { 0 };

	ec_range(ec, n, &min, &max, &mean, &valid);
	doc_style(d, S_TITLE);
	doc_printf(d, "Erase counters\n");
	doc_style(d, S_NORMAL);
	if (!valid) {
		doc_printf(d, "  no valid erase counter\n");
	} else {
		doc_printf(d, "  %d valid, min %lld, mean %lld, max %lld, spread %lld\n",
			   valid, min, mean, max, max - min);
		for (i = 0; i < n; i++)
			if (ec[i] >= 0)
				cnt[ec_bucket(ec[i], min, max)]++;
		doc_printf(d, "\n");
		doc_style(d, S_TITLE);
		doc_printf(d, "Histogram\n");
		doc_style(d, S_NORMAL);
		for (i = 0; i < 5; i++) {
			long long lo = min + (max - min + 1) * i / 5;
			long long hi = min + (max - min + 1) * (i + 1) / 5 - 1;
			int bar = valid ? cnt[i] * (width - 34) / valid : 0;

			if (max <= min && i)
				break;
			if (max <= min)
				hi = max;
			doc_printf(d, "  ");
			doc_style(d, ec_styles[i]);
			doc_printf(d, "%d", i);
			doc_style(d, S_NORMAL);
			doc_printf(d, " %8lld..%-8lld %6d ", lo, hi, cnt[i]);
			doc_style(d, ec_styles[i]);
			for (j = 0; j < bar; j++)
				doc_printf(d, term_mono ? "#" : " ");
			doc_style(d, S_NORMAL);
			doc_printf(d, "\n");
		}
	}
	doc_printf(d, "\n");
	doc_style(d, S_TITLE);
	doc_printf(d, "PEB map");
	doc_style(d, S_NORMAL);
	doc_printf(d, "  (digit: histogram bucket, X bad, ? unknown, . empty, "
		   "C corrupted, A alien)\n");
	per = (width - 8) / 16 * 16;
	if (per < 16)
		per = 16;
	for (i = 0; i < n; i += per) {
		doc_printf(d, "%6d ", i);
		for (j = i; j < n && j < i + per; j++) {
			long long v = ec[j];

			switch (v) {
			case PEB_BAD:
				doc_style(d, S_ERR);
				doc_printf(d, "X");
				break;
			case PEB_UNKNOWN:
				doc_style(d, S_DIM);
				doc_printf(d, "?");
				break;
			case PEB_EMPTY:
				doc_style(d, S_DIM);
				doc_printf(d, ".");
				break;
			case PEB_CORRUPT:
				doc_style(d, S_ERR);
				doc_printf(d, "C");
				break;
			case PEB_ALIEN:
				doc_style(d, S_WARN);
				doc_printf(d, "A");
				break;
			default:
				doc_style(d, ec_styles[ec_bucket(v, min, max)]);
				doc_printf(d, "%d", ec_bucket(v, min, max));
			}
		}
		doc_style(d, S_NORMAL);
		doc_printf(d, "\n");
	}
}

struct scanview {
	struct mtd_dev_info mtd;
	struct scan s;
};

static void scan_build(struct doc *d, int width, void *ctx)
{
	struct scanview *v = ctx;
	char a[32];

	doc_style(d, S_TITLE);
	doc_printf(d, "mtd%d \"%s\"\n", v->mtd.mtd_num, v->mtd.name);
	doc_style(d, S_NORMAL);
	doc_printf(d, "  %s, %s, %d eraseblocks of %d bytes, min. I/O %d bytes\n\n",
		   v->mtd.type_str, fmt_size(v->mtd.size, a, sizeof(a)),
		   v->mtd.eb_cnt, v->mtd.eb_size, v->mtd.min_io_size);
	doc_style(d, S_TITLE);
	doc_printf(d, "Eraseblocks\n");
	doc_style(d, S_NORMAL);
	doc_printf(d, "  valid UBI EC header %6d\n", v->s.ok);
	doc_printf(d, "  empty (all 0xFF)    %6d\n", v->s.empty);
	doc_printf(d, "  corrupted header    %6d\n", v->s.corrupted);
	doc_printf(d, "  alien (not UBI)     %6d\n", v->s.alien);
	doc_printf(d, "  bad                 %6d\n", v->s.bad);
	if (v->s.vid_hdr_offs >= 0)
		doc_printf(d, "  VID header offset %d, data offset %d\n",
			   v->s.vid_hdr_offs, v->s.data_offs);
	doc_printf(d, "\n");
	ec_doc(d, width, v->s.ec, v->s.eb_cnt);
}

void scan_view(int mtd_num)
{
	struct scanview v;
	int fd, r;

	op_begin();
	fd = open_mtd(mtd_num, O_RDONLY, &v.mtd);
	if (fd < 0) {
		op_fail("Scan", errno, "Cannot open mtd%d.", mtd_num);
		return;
	}
	r = mtd_scan(&v.mtd, fd, &v.s);
	close(fd);
	if (r < 0) {
		op_fail("Scan", errno, "Scanning mtd%d failed.", mtd_num);
		return;
	}
	if (r > 0)
		return;
	ui_view("Scan of UBI headers (ubiscan)", scan_build, &v);
	free(v.s.ec);
}

/* ---- attached device: erase counters and read check ---- */

struct wearview {
	const struct dev *d;
	long long *ec;
	int n;
};

static void wear_build(struct doc *doc, int width, void *ctx)
{
	struct wearview *v = ctx;
	int bad = 0, unknown = 0, i;

	for (i = 0; i < v->n; i++) {
		bad += v->ec[i] == PEB_BAD;
		unknown += v->ec[i] == PEB_UNKNOWN;
	}
	doc_style(doc, S_TITLE);
	doc_printf(doc, "ubi%d on mtd%d: %d PEBs\n", v->d->num, v->d->info.mtd_num,
		   v->n);
	doc_style(doc, S_NORMAL);
	doc_printf(doc, "  Live values from the wear-leveling subsystem (UBI_IOCECNFO).\n"
		   "  Unknown: %d (not tracked, e.g. reserved or being erased)%s.\n\n",
		   unknown, bad ? "" : ", no bad PEBs");
	ec_doc(doc, width, v->ec, v->n);
}

void wear_view(const struct dev *d)
{
	struct wearview v = { .d = d };
	struct ubi_ecinfo_req *req;
	char node[32];
	int fd, chunk = 256, start = 0, i;

	if (!d->have_mtd) {
		ui_error("Erase counters", "The MTD device of ubi%d is unknown.", d->num);
		return;
	}
	v.n = d->mtd.eb_cnt;
	v.ec = calloc(v.n, sizeof(*v.ec));
	req = malloc(sizeof(*req) + chunk * sizeof(int32_t));
	dev_node(d->num, node, sizeof(node));
	op_begin();
	fd = open(node, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || !v.ec || !req) {
		op_fail("Erase counters", errno, "Cannot open %s.", node);
		goto out;
	}
	while (start < v.n) {
		memset(req, 0, sizeof(*req));
		req->start = start;
		req->length = chunk;
		if (ioctl(fd, UBI_IOCECNFO, req) || req->read_length <= 0) {
			int e = errno;

			close(fd);
			if (e == ENOTTY) {
				ui_error("Erase counters",
					 "This kernel has no UBI_IOCECNFO. Use Devices > "
					 "Scan instead, which reads the erase counters "
					 "from the flash.");
				goto out;
			}
			op_fail("Erase counters", e, "UBI_IOCECNFO failed at PEB %d.",
				start);
			goto out;
		}
		for (i = 0; i < req->read_length && start + i < v.n; i++)
			v.ec[start + i] = req->erase_counters[i] < 0 ?
						  PEB_UNKNOWN : req->erase_counters[i];
		start += req->read_length;
	}
	close(fd);
	if (lmtd) {
		struct mtd_dev_info mi;
		int mfd = open_mtd(d->info.mtd_num, O_RDONLY, &mi);

		if (mfd >= 0) {
			for (i = 0; i < v.n; i++)
				if (v.ec[i] == PEB_UNKNOWN && mtd_is_bad(&mi, mfd, i) > 0)
					v.ec[i] = PEB_BAD;
			close(mfd);
		}
	}
	ui_view("Erase counters (wear)", wear_build, &v);
out:
	free(req);
	free(v.ec);
}

struct checkres {
	int pebs, ok, scrubbed, busy, cannot, ignored, failed;
	char failures[2048];
};

static void check_build(struct doc *d, int width, void *ctx)
{
	struct checkres *r = ctx;

	(void)width;
	doc_style(d, S_TITLE);
	doc_printf(d, "Read check of %d PEBs\n", r->pebs);
	doc_style(d, S_NORMAL);
	doc_printf(d, "  read without bit-flips            %6d\n", r->ok);
	doc_style(d, r->scrubbed ? S_WARN : S_NORMAL);
	doc_printf(d, "  bit-flips, scrubbed (data moved)  %6d\n", r->scrubbed);
	doc_style(d, r->cannot ? S_ERR : S_NORMAL);
	doc_printf(d, "  bit-flips, cannot scrub now       %6d\n", r->cannot);
	doc_style(d, S_NORMAL);
	doc_printf(d, "  busy (in use by UBI right now)    %6d\n", r->busy);
	doc_printf(d, "  not used by UBI (free/bad/resvd)  %6d\n", r->ignored);
	doc_style(d, r->failed ? S_ERR : S_NORMAL);
	doc_printf(d, "  other errors                      %6d\n", r->failed);
	doc_style(d, S_NORMAL);
	if (*r->failures)
		doc_printf(d, "\n%s", r->failures);
	doc_printf(d, "\nUncorrectable ECC errors are not reported by this "
		   "interface; check the kernel log.\n");
}

void health_check(const struct dev *d)
{
	struct checkres r = { 0 };
	struct progress p;
	char node[32];
	int fd, pnum;

	if (!d->have_mtd) {
		ui_error("Read check", "The MTD device of ubi%d is unknown.", d->num);
		return;
	}
	if (!ui_yesno("Read check",
		      "Read every PEB of the device and let UBI scrub (move) those "
		      "with correctable bit-flips, as ubihealthd does. This reads "
		      "the whole flash and may take a while. Continue?", 1))
		return;
	dev_node(d->num, node, sizeof(node));
	op_begin();
	fd = open(node, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		op_fail("Read check", errno, "Cannot open %s.", node);
		return;
	}
	r.pebs = d->mtd.eb_cnt;
	prog_start(&p, "Read check", r.pebs, 1);
	p.is_bytes = 0;
	for (pnum = 0; pnum < r.pebs; pnum++) {
		int32_t arg = pnum;

		if (prog_update(&p, pnum, "PEB %d: %d scrubbed so far", pnum,
				r.scrubbed)) {
			r.pebs = pnum;
			break;
		}
		if (!ioctl(fd, UBI_IOCRPEB, &arg)) {
			r.ok++;
			continue;
		}
		switch (errno) {
		case EUCLEAN: r.scrubbed++; break;
		case EBUSY: r.busy++; break;
		case EAGAIN: r.cannot++; break;
		case ENOENT: r.ignored++; break;
		case ENOTTY:
			prog_end(&p);
			close(fd);
			ui_error("Read check", "This kernel has no UBI_IOCRPEB.");
			return;
		default: {
			size_t n = strlen(r.failures);

			r.failed++;
			if (n < sizeof(r.failures) - 64)
				snprintf(r.failures + n, sizeof(r.failures) - n,
					 "  PEB %d: %s\n", pnum, strerror(errno));
		}
		}
	}
	prog_end(&p);
	close(fd);
	ui_view("Read check result", check_build, &r);
}

/* ---- ubiformat ---- */

struct fmt {
	libmtd_t lib;
	struct mtd_dev_info mtd;
	int fd;
	struct scan s;
	int override_ec;
	long long ec;
	int prev_bad, consecutive_bad;
	struct progress *p;
};

static int fmt_mark_bad(struct fmt *f, int eb, const char *what)
{
	char text[256];

	snprintf(text, sizeof(text), "%s eraseblock %d failed with an I/O "
		 "error. Mark it bad?", what, eb);
	if (!ui_yesno("Bad eraseblock", text, 0))
		return -1;
	if (!f->mtd.bb_allowed) {
		ui_error("Format", "Bad blocks are not supported by this flash.");
		return -1;
	}
	if (mtd_mark_bad(&f->mtd, f->fd, eb)) {
		op_fail("Format", errno, "Cannot mark eraseblock %d bad.", eb);
		return -1;
	}
	f->s.bad++;
	f->s.ec[eb] = PEB_BAD;
	f->consecutive_bad = f->prev_bad >= 0 && eb == f->prev_bad + 1 ?
				     f->consecutive_bad + 1 : 1;
	f->prev_bad = eb;
	if (f->consecutive_bad >= MAX_CONSECUTIVE_BAD_BLOCKS) {
		ui_error("Format", "%d consecutive bad eraseblocks: bad flash or "
			 "driver? Stopped.", MAX_CONSECUTIVE_BAD_BLOCKS);
		return -1;
	}
	return 0;
}

static long long fmt_ec(const struct fmt *f, int eb)
{
	if (f->override_ec)
		return f->ec;
	if (f->s.ec[eb] >= 0)
		return f->s.ec[eb] + 1;
	return f->s.mean;
}

static int change_ech(struct ubi_ec_hdr *hdr, uint32_t image_seq, long long ec)
{
	if (be32_to_cpu(hdr->magic) != UBI_EC_HDR_MAGIC ||
	    be32_to_cpu(hdr->hdr_crc) !=
		    mtd_crc32(UBI_CRC32_INIT, hdr, UBI_EC_HDR_SIZE_CRC))
		return -1;
	hdr->image_seq = cpu_to_be32(image_seq);
	hdr->ec = cpu_to_be64(ec);
	hdr->hdr_crc = cpu_to_be32(mtd_crc32(UBI_CRC32_INIT, hdr,
					     UBI_EC_HDR_SIZE_CRC));
	return 0;
}

static int drop_ffs(const struct mtd_dev_info *mtd, const unsigned char *buf,
		    int len)
{
	int i;

	for (i = len - 1; i >= 0; i--)
		if (buf[i] != 0xff)
			break;
	len = i + 1;
	return (len + mtd->min_io_size - 1) / mtd->min_io_size * mtd->min_io_size;
}

/* Writes the UBI image eraseblock by eraseblock. Returns the next PEB to
 * format, or -1. */
static int fmt_flash_image(struct fmt *f, const struct ubigen_info *ui,
			   struct src *in, long long size)
{
	int img_ebs = size / f->mtd.eb_size, eb, written = 0, skip_read = 0;
	unsigned char *buf = malloc(f->mtd.eb_size);

	if (!buf)
		return -1;
	for (eb = 0; eb < f->mtd.eb_cnt; eb++) {
		int len;

		prog_update(f->p, written, "Flashing eraseblock %d", eb);
		if (f->s.ec[eb] == PEB_BAD)
			continue;
		if (mtd_erase(f->lib, &f->mtd, f->fd, eb)) {
			if (errno != EIO) {
				op_fail("Format", errno, "Cannot erase eraseblock %d.", eb);
				goto fail;
			}
			if (fmt_mark_bad(f, eb, "Erasing"))
				goto fail;
			continue;
		}
		if (!skip_read && src_read(in, buf, f->mtd.eb_size) != f->mtd.eb_size) {
			op_fail("Format", errno, "The image ends early (eraseblock %d of "
				"%d).", written, img_ebs);
			goto fail;
		}
		skip_read = 0;
		if (change_ech((struct ubi_ec_hdr *)buf, ui->image_seq, fmt_ec(f, eb))) {
			ui_error("Format", "Bad EC header in eraseblock %d of the image: "
				 "not a UBI image (made by ubinize)?", written);
			goto fail;
		}
		len = drop_ffs(&f->mtd, buf, f->mtd.eb_size);
		if (mtd_write(f->lib, &f->mtd, f->fd, eb, 0, buf, len, NULL, 0, 0)) {
			if (errno != EIO) {
				op_fail("Format", errno, "Cannot write eraseblock %d.", eb);
				goto fail;
			}
			if (mtd_torture(f->lib, &f->mtd, f->fd, eb) &&
			    fmt_mark_bad(f, eb, "Writing"))
				goto fail;
			skip_read = 1; /* write the same data to the next PEB */
			continue;
		}
		if (++written >= img_ebs)
			break;
	}
	free(buf);
	if (written < img_ebs) {
		ui_error("Format", "Only %d of %d image eraseblocks fit.", written,
			 img_ebs);
		return -1;
	}
	return eb + 1;
fail:
	free(buf);
	return -1;
}

static int fmt_format(struct fmt *f, const struct ubigen_info *ui, int start,
		      int novtbl)
{
	int write_size, eb, eb1 = -1, eb2 = -1;
	long long ec1 = -1, ec2 = -1;
	struct ubi_ec_hdr *hdr;
	struct ubi_vtbl_record *vtbl;

	write_size = (UBI_EC_HDR_SIZE + f->mtd.subpage_size - 1) /
		     f->mtd.subpage_size * f->mtd.subpage_size;
	hdr = malloc(write_size);
	if (!hdr)
		return -1;
	memset(hdr, 0xff, write_size);
	for (eb = start; eb < f->mtd.eb_cnt; eb++) {
		long long ec;

		prog_update(f->p, eb, "Formatting eraseblock %d", eb);
		if (f->s.ec[eb] == PEB_BAD)
			continue;
		ec = fmt_ec(f, eb);
		ubigen_init_ec_hdr(ui, hdr, ec);
		if (mtd_erase(f->lib, &f->mtd, f->fd, eb)) {
			if (errno != EIO) {
				op_fail("Format", errno, "Cannot erase eraseblock %d.", eb);
				goto fail;
			}
			if (fmt_mark_bad(f, eb, "Erasing"))
				goto fail;
			continue;
		}
		if (!novtbl && (eb1 < 0 || eb2 < 0)) {
			if (eb1 < 0) {
				eb1 = eb;
				ec1 = ec;
			} else {
				eb2 = eb;
				ec2 = ec;
			}
			continue; /* the volume table goes here */
		}
		if (mtd_write(f->lib, &f->mtd, f->fd, eb, 0, hdr, write_size, NULL, 0,
			      0)) {
			if (errno != EIO) {
				op_fail("Format", errno, "Cannot write the EC header to "
					"eraseblock %d (%d bytes; wrong sub-page size?).",
					eb, write_size);
				goto fail;
			}
			if (mtd_torture(f->lib, &f->mtd, f->fd, eb) &&
			    fmt_mark_bad(f, eb, "Writing"))
				goto fail;
		}
	}
	prog_update(f->p, f->mtd.eb_cnt, "Writing the volume table");
	if (!novtbl) {
		if (eb1 < 0 || eb2 < 0) {
			ui_error("Format", "No eraseblocks left for the volume table.");
			goto fail;
		}
		vtbl = ubigen_create_empty_vtbl(ui);
		if (!vtbl || ubigen_write_layout_vol(ui, eb1, eb2, ec1, ec2, vtbl,
						     f->fd)) {
			free(vtbl);
			op_fail("Format", errno, "Cannot write the volume table.");
			goto fail;
		}
		free(vtbl);
	}
	free(hdr);
	return 0;
fail:
	free(hdr);
	return -1;
}

static const char *const ec_modes[] = { "keep (old EC + 1)", "set to value",
					 NULL };

static int fmt_check(struct field *f, int n, char *note, size_t sz, void *ctx)
{
	const struct mtd_dev_info *mtd = ctx;
	char *end;
	long v;

	(void)n;
	/* The size is needed when the input does not tell it. */
	f[2].hidden = strcmp(f[0].buf, "-") && strncmp(f[0].buf, "http://", 7);
	f[5].hidden = f[4].choice == 0;
	if (f[3].buf[0]) {
		v = strtol(f[3].buf, &end, 0);
		if (*end || v <= 0 || (v & (v - 1)) || v > mtd->min_io_size ||
		    mtd->min_io_size % v) {
			snprintf(note, sz, "Sub-page size: a power of 2 dividing the "
				 "min. I/O size %d.", mtd->min_io_size);
			return 1;
		}
	}
	if (f[6].buf[0]) {
		v = strtol(f[6].buf, &end, 0);
		if (*end || v <= 0 || v % 8 || v + (long)UBI_VID_HDR_SIZE > mtd->eb_size) {
			snprintf(note, sz, "VID header offset: a multiple of 8 inside "
				 "the eraseblock, or empty for the default.");
			return 1;
		}
	}
	if (!f[5].hidden) {
		long long e = strtoll(f[5].buf, &end, 0);

		if (!f[5].buf[0] || *end || e < 0 || e >= UBI_MAX_ERASECOUNTER) {
			snprintf(note, sz, "Erase counter: 0 to %d.",
				 UBI_MAX_ERASECOUNTER - 1);
			return 1;
		}
	}
	if (f[7].buf[0]) {
		unsigned long long q = strtoull(f[7].buf, &end, 0);

		if (*end || q > 0xffffffffULL) {
			snprintf(note, sz, "Image sequence: a 32-bit number or empty "
				 "for a random one.");
			return 1;
		}
	}
	snprintf(note, sz, "Everything on mtd%d \"%s\" (%d eraseblocks) will be "
		 "erased.%s", mtd->mtd_num, mtd->name, mtd->eb_cnt,
		 f[0].buf[0] ? " The image must be made by ubinize for this flash "
			       "geometry." : "");
	return 0;
}

int format_mtd(int mtd_num)
{
	struct field fl[8] = {
		{ .type = F_TEXT, .label = "UBI image:", .browse = 1,
		  .help = "ubinize image: file, http://host:port/path or - (stdin); "
			  "empty: none" },
		{ .type = F_TEXT, .label = "UBI version:", .maxlen = 3,
		  .help = "Version number in the EC headers (ubiformat -x)" },
		{ .type = F_TEXT, .label = "Image size:", .maxlen = 20,
		  .help = "Bytes, required when the size is not known (stdin)" },
		{ .type = F_TEXT, .label = "Sub-page size:", .maxlen = 8,
		  .help = "Minimum unit for UBI headers (ubiformat -s)" },
		{ .type = F_CHOICE, .label = "Erase counters:", .choices = ec_modes,
		  .help = "Keep: preserve wear information (recommended)" },
		{ .type = F_TEXT, .label = "EC value:", .maxlen = 10,
		  .help = "Erase counter written to every PEB (ubiformat -e)" },
		{ .type = F_TEXT, .label = "VID hdr offset:", .maxlen = 8,
		  .help = "Empty = default (ubiformat -O); change only if you must" },
		{ .type = F_TEXT, .label = "Image sequence:", .maxlen = 12,
		  .help = "32-bit image sequence number; empty = random (ubiformat -Q)" },
	};
	struct fmt f = { .fd = -1, .prev_bad = -1 };
	struct ubigen_info ui;
	struct progress p;
	struct src in = { .fd = -1 };
	char text[2048], err[512], a[32];
	long long img_size = 0;
	int subpage, vid_offs = 0, ubi_ver, dev, r, start;
	uint32_t image_seq;

	f.lib = lmtd;
	op_begin();
	if (!lmtd || mtd_get_dev_info1(lmtd, mtd_num, &f.mtd)) {
		op_fail("Format", errno, "Cannot read mtd%d.", mtd_num);
		return -1;
	}
	if (lubi && !mtd_num2ubi_dev(lubi, mtd_num, &dev)) {
		ui_error("Format", "mtd%d is attached to ubi%d. Detach it first.",
			 mtd_num, dev);
		return -1;
	}
	if (!f.mtd.writable) {
		ui_error("Format", "mtd%d is read-only.", mtd_num);
		return -1;
	}
	if (f.mtd.min_io_size & (f.mtd.min_io_size - 1)) {
		ui_error("Format", "The min. I/O size %d is not a power of 2.",
			 f.mtd.min_io_size);
		return -1;
	}
	snprintf(fl[1].buf, sizeof(fl[1].buf), "1");
	snprintf(fl[3].buf, sizeof(fl[3].buf), "%d", f.mtd.subpage_size);
	snprintf(text, sizeof(text), "Format mtd%d \"%s\" (%s, %s, %d eraseblocks of "
		 "%s, min. I/O %d) with UBI, like ubiformat.", mtd_num, f.mtd.name,
		 f.mtd.type_str, fmt_size(f.mtd.size, a, sizeof(a)), f.mtd.eb_cnt,
		 fmt_size(f.mtd.eb_size, err, sizeof(err)), f.mtd.min_io_size);
	if (!ui_form("Format MTD device (ubiformat)", text, fl, 8, fmt_check, &f.mtd))
		return -1;

	ubi_ver = atoi(fl[1].buf);
	subpage = fl[3].buf[0] ? (int)strtol(fl[3].buf, NULL, 0) : f.mtd.subpage_size;
	f.mtd.subpage_size = subpage;
	if (fl[6].buf[0])
		vid_offs = strtol(fl[6].buf, NULL, 0);
	if (fl[4].choice) {
		f.override_ec = 1;
		f.ec = strtoll(fl[5].buf, NULL, 0);
	}
	if (fl[7].buf[0]) {
		image_seq = strtoul(fl[7].buf, NULL, 0);
	} else {
		srand(time(NULL) ^ getpid());
		image_seq = (uint32_t)rand() << 1 ^ (uint32_t)rand();
	}

	if (fl[0].buf[0]) {
		if (src_open(&in, fl[0].buf, 0, err, sizeof(err))) {
			ui_error("Format", "%s", err);
			return -1;
		}
		img_size = in.size;
		if (!fl[2].hidden && fl[2].buf[0])
			img_size = strtoll(fl[2].buf, NULL, 0);
		if (img_size <= 0 || img_size % f.mtd.eb_size) {
			ui_error("Format", "The image size (%lld) must be known and a "
				 "multiple of the eraseblock size %d.", img_size,
				 f.mtd.eb_size);
			src_close(&in);
			return -1;
		}
	}

	f.fd = open_mtd(mtd_num, O_RDWR, &f.mtd);
	f.mtd.subpage_size = subpage;
	if (f.fd < 0) {
		op_fail("Format", errno, "Cannot open mtd%d.", mtd_num);
		src_close(&in);
		return -1;
	}
	r = mtd_scan(&f.mtd, f.fd, &f.s);
	if (r) {
		if (r < 0)
			op_fail("Format", errno, "Scanning mtd%d failed.", mtd_num);
		goto out;
	}
	if (f.s.good < 2) {
		ui_error("Format", "Too few good eraseblocks (%d).", f.s.good);
		goto out;
	}
	if (img_size / f.mtd.eb_size > f.s.good) {
		ui_error("Format", "The image (%lld eraseblocks) is larger than the "
			 "%d good eraseblocks.", img_size / f.mtd.eb_size, f.s.good);
		goto out;
	}
	r = snprintf(text, sizeof(text), "Scan of mtd%d:\n  %d valid erase "
		     "counters (mean %lld)\n  %d empty\n  %d corrupted\n  %d alien "
		     "(not UBI)\n  %d bad\n", mtd_num, f.s.ok, f.s.mean, f.s.empty,
		     f.s.corrupted, f.s.alien, f.s.bad);
	if (f.s.alien)
		r += snprintf(text + r, sizeof(text) - r, "\nWarning: %d of %d "
			      "eraseblocks contain non-UBI data, which will be "
			      "lost.\n", f.s.alien, f.s.good);
	if (!f.override_ec && f.s.empty < f.s.good) {
		int percent = f.s.ok * 100 / f.s.good;

		if (percent < 50) {
			r += snprintf(text + r, sizeof(text) - r, "\nOnly %d of %d "
				      "eraseblocks have a valid erase counter: erase "
				      "counter 0 will be used for all.\n", f.s.ok, f.s.good);
			f.override_ec = 1;
			f.ec = 0;
		} else if (percent < 95) {
			r += snprintf(text + r, sizeof(text) - r, "\nOnly %d of %d "
				      "eraseblocks have a valid erase counter: the mean "
				      "%lld will be used for all.\n", f.s.ok, f.s.good,
				      f.s.mean);
			f.override_ec = 1;
			f.ec = f.s.mean;
		}
	}
	if (f.override_ec)
		r += snprintf(text + r, sizeof(text) - r, "\nErase counter %lld for "
			      "all eraseblocks.\n", f.ec);
	ubigen_info_init(&ui, f.mtd.eb_size, f.mtd.min_io_size, f.mtd.subpage_size,
			 vid_offs, ubi_ver, image_seq);
	if (f.s.vid_hdr_offs != -1 && ui.vid_hdr_offs != f.s.vid_hdr_offs) {
		char q[400];

		snprintf(q, sizeof(q), "The flash has VID header and data offsets %d "
			 "and %d, the new ones would be %d and %d. Use the new offsets? "
			 "(No keeps the old ones.)", f.s.vid_hdr_offs, f.s.data_offs,
			 ui.vid_hdr_offs, ui.data_offs);
		if (!ui_yesno("Format", q, 1))
			ubigen_info_init(&ui, f.mtd.eb_size, f.mtd.min_io_size, 0,
					 f.s.vid_hdr_offs, ubi_ver, image_seq);
	}
	snprintf(text + r, sizeof(text) - r, "\nVID header offset %d, data offset "
		 "%d, LEB size %d, image sequence 0x%08x.\n%s\nERASE all of mtd%d "
		 "\"%s\"?", ui.vid_hdr_offs, ui.data_offs, ui.leb_size, ui.image_seq,
		 fl[0].buf[0] ? "The UBI image is written first." : "", mtd_num,
		 f.mtd.name);
	if (!ui_confirm_yes("Format MTD device", text))
		goto out;

	prog_start(&p, "Formatting", f.mtd.eb_cnt, 0);
	p.is_bytes = 0;
	f.p = &p;
	start = 0;
	if (fl[0].buf[0]) {
		p.total = img_size / f.mtd.eb_size;
		start = fmt_flash_image(&f, &ui, &in, img_size);
		p.total = f.mtd.eb_cnt;
	}
	r = start < 0 ? -1 : fmt_format(&f, &ui, start, fl[0].buf[0] != 0);
	prog_end(&p);
	if (!r) {
		snprintf(text, sizeof(text), "mtd%d is formatted. Attach it to UBI "
			 "now?", mtd_num);
		r = ui_yesno("Format done", text, 1) ? 1 : 0;
	}
	close(f.fd);
	f.fd = -1;
	free(f.s.ec);
	src_close(&in);
	return r;
out:
	free(f.s.ec);
	if (f.fd >= 0)
		close(f.fd);
	src_close(&in);
	return -1;
}
