/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UBITUI_UBI_H
#define UBITUI_UBI_H

#include <stddef.h>
#include <stdint.h>

#include <libmtd.h>
#include <libubi.h>

#define MAX_VOLS 128

struct vol {
	struct ubi_vol_info info;
	int upd_marker;
	int mapped; /* mapped LEBs, -1 if unknown */
	int block;  /* a ubiblock device exists */
	char use[160]; /* mounts using the volume, "" if none found */
};

struct dev {
	int num;
	struct ubi_dev_info info;
	struct mtd_dev_info mtd;
	int have_mtd;
	int ro_mode;
	int nvols;
	struct vol vols[MAX_VOLS];
};

extern libubi_t lubi;
extern libmtd_t lmtd;

void backend_open(void);
int dev_load(struct dev *d, int num);
void dev_node(int num, char *buf, size_t sz);
void vol_node(int dev, int vol, char *buf, size_t sz);
int vol_find_name(const struct dev *d, const char *name);
int sysfs_int(const char *fmt, int a, int b, int def);

/* "123", "4 KiB", "1.5M", "96 LEB", "max" (max_bytes). Returns bytes or -1
 * with err set. *lebs is set to the LEB count for LEB sizes, else -1. */
long long parse_size(const char *s, long long leb, long long max_bytes,
		     long long *lebs, char *err, size_t esz);

/* Library messages (stderr) and kernel log lines are collected per
 * operation and shown when it fails. */
void errcap_init(void);
void op_begin(void);
void op_fail(const char *title, int err, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* Input for writes and formatting: a file or device, "-" (stdin, when it is
 * not the terminal) or an http:// URL. */
struct src {
	int fd;
	long long size; /* -1 if unknown */
	char pre[8192]; /* body bytes read with the HTTP headers (<= hdr) */
	int prelen, preoff;
};
int src_open(struct src *s, const char *spec, long long skip, char *err,
	     size_t esz);
/* Reads n bytes unless the input ends first. Returns the count or -1. */
long long src_read(struct src *s, void *buf, long long n);
void src_close(struct src *s);

#endif
