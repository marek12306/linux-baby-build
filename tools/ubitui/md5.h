/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UBITUI_MD5_H
#define UBITUI_MD5_H

#include <stddef.h>
#include <stdint.h>

struct md5 {
	uint32_t s[4];
	uint64_t len;
	unsigned char buf[64];
};

void md5_init(struct md5 *m);
void md5_update(struct md5 *m, const void *data, size_t n);
/* Finishes and writes 32 lowercase hex digits plus NUL. */
void md5_hex(struct md5 *m, char out[33]);

#endif
