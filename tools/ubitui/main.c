// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ubitui - a cfdisk-like terminal UI for UBI.
 *
 * Shows the volumes of a UBI device as a table and a usage bar and performs
 * what the mtd-utils ubi-utils do: create (ubimkvol), remove (ubirmvol),
 * resize (ubirsvol), rename atomically (ubirename), write (ubiupdatevol),
 * attach/detach (ubiattach/ubidetach), format (ubiformat), ubiblock, ubinfo,
 * mtdinfo, ubiscan and ubicrc32, plus backups, checksums, copies, a hex view,
 * LEB and erase counter maps and a bit-flip read check.
 *
 * Every change is done immediately through the UBI ioctls, except renames:
 * they are collected and applied together in one atomic UBI_IOCRNVOL, which
 * also removes a volume whose name is taken over (as ubirename does).
 */
#include "flash.h"
#include "md5.h"
#include "ubi.h"
#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <crc32.h>
#include <mtd/ubi-media.h> /* before libubigen.h, which uses its structs */
#include <libubigen.h>

#define VERSION "1.0"
#define UBI_CTRL "/dev/ubi_ctrl"

struct rename {
	int vol_id;
	char from[UBI_VOL_NAME_MAX + 1];
	char to[UBI_VOL_NAME_MAX + 1];
};

enum act {
	ACT_INFO, ACT_NEW, ACT_DELETE, ACT_RESIZE, ACT_RENAME, ACT_WRITE,
	ACT_BACKUP, ACT_EXTRA, ACT_DEVICES, ACT_APPLY, ACT_UNDO, ACT_HELP,
	ACT_QUIT, ACT_NONE,
};

struct menu_item {
	const char *label;
	int hot;
	enum act act;
	const char *help;
};

static const struct menu_item all_items[] = {
	{ "Info", 'i', ACT_INFO, "Details of the volume (ubinfo), its LEB map" },
	{ "New", 'n', ACT_NEW, "Create a volume in the free space (ubimkvol)" },
	{ "Delete", 'd', ACT_DELETE, "Remove the volume and its data (ubirmvol)" },
	{ "Resize", 's', ACT_RESIZE, "Grow or shrink the volume (ubirsvol)" },
	{ "Rename", 'r', ACT_RENAME, "Stage a new name; applied atomically (ubirename)" },
	{ "Write", 'w', ACT_WRITE, "Replace the contents with an image (ubiupdatevol)" },
	{ "Backup", 'b', ACT_BACKUP, "Save the contents to a file" },
	{ "Extra", 'x', ACT_EXTRA, "Copy, checksum, hex view, wipe, ubiblock, wear map..." },
	{ "Devices", 'v', ACT_DEVICES, "MTD and UBI devices: attach, detach, format, scan" },
	{ "Apply", 'a', ACT_APPLY, "Apply all staged renames in one atomic operation" },
	{ "Undo", 'u', ACT_UNDO, "Forget the staged renames" },
	{ "Help", 'h', ACT_HELP, "Keys and the mtd-utils equivalent of every operation" },
	{ "Quit", 'q', ACT_QUIT, "Leave ubitui" },
};

static struct dev D;
static int have_dev;
static int sel, top, menu_sel;
static struct rename ren[UBI_MAX_RNVOL];
static int nren;
static char status[256];
static int status_style;
static int quit;

static int imin(int a, int b)
{
	return a < b ? a : b;
}

static int imax(int a, int b)
{
	return a > b ? a : b;
}

static void set_status(int style, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
static void set_status(int style, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(status, sizeof(status), fmt, ap);
	va_end(ap);
	status_style = style;
}

static struct vol *cur_vol(void)
{
	return have_dev && sel < D.nvols ? &D.vols[sel] : NULL;
}

static const struct rename *ren_for(int vol_id)
{
	int i;

	for (i = 0; i < nren; i++)
		if (ren[i].vol_id == vol_id)
			return &ren[i];
	return NULL;
}

/* A volume whose name another volume takes over in the staged renames, and
 * which is not renamed itself: UBI removes it when the renames are applied. */
static int removed_by_rename(const struct vol *v)
{
	int i;

	if (ren_for(v->info.vol_id))
		return 0;
	for (i = 0; i < nren; i++)
		if (!strcmp(ren[i].to, v->info.name))
			return 1;
	return 0;
}

static void ren_drop(int vol_id)
{
	int i;

	for (i = 0; i < nren; i++) {
		if (ren[i].vol_id == vol_id) {
			ren[i] = ren[--nren];
			return;
		}
	}
}

static void reload(void)
{
	int vol_id = cur_vol() ? cur_vol()->info.vol_id : -1, i;
	int was_free = have_dev && sel >= D.nvols;

	if (!have_dev)
		return;
	if (dev_load(&D, D.num)) {
		have_dev = 0;
		nren = 0;
		return;
	}
	/* Staged renames of volumes that changed meanwhile are dropped. */
	for (i = 0; i < nren;) {
		int k;

		for (k = 0; k < D.nvols; k++)
			if (D.vols[k].info.vol_id == ren[i].vol_id &&
			    !strcmp(D.vols[k].info.name, ren[i].from))
				break;
		if (k == D.nvols)
			ren[i] = ren[--nren];
		else
			i++;
	}
	sel = D.nvols;
	if (!was_free) {
		for (i = 0; i < D.nvols; i++)
			if (D.vols[i].info.vol_id == vol_id)
				sel = i;
	}
}

static void select_vol(int vol_id)
{
	int i;

	for (i = 0; i < D.nvols; i++)
		if (D.vols[i].info.vol_id == vol_id)
			sel = i;
}

static long long content_end(const struct vol *v);

static long long sum_rsvd(void)
{
	long long s = 0;
	int i;

	for (i = 0; i < D.nvols; i++)
		s += D.vols[i].info.rsvd_lebs;
	return s;
}

static int next_free_id(void)
{
	int id, i;

	for (id = 0; id < D.info.max_vol_count; id++) {
		for (i = 0; i < D.nvols; i++)
			if (D.vols[i].info.vol_id == id)
				break;
		if (i == D.nvols)
			return id;
	}
	return -1;
}

/* ---- main screen ---- */

static int build_menu(const struct menu_item **out)
{
	static const enum act vol_acts[] = { ACT_INFO, ACT_DELETE, ACT_RESIZE,
		ACT_RENAME, ACT_WRITE, ACT_BACKUP, ACT_EXTRA, ACT_DEVICES, ACT_HELP,
		ACT_QUIT };
	static const enum act free_acts[] = { ACT_INFO, ACT_NEW, ACT_EXTRA,
		ACT_DEVICES, ACT_HELP, ACT_QUIT };
	const enum act *acts = cur_vol() ? vol_acts : free_acts;
	int na = cur_vol() ? 10 : 6, n = 0, i;

	if (nren) {
		out[n++] = &all_items[ACT_APPLY];
		out[n++] = &all_items[ACT_UNDO];
	}
	for (i = 0; i < na; i++)
		out[n++] = &all_items[acts[i]];
	return n;
}

static int name_width(void)
{
	int w = 10, i;

	for (i = 0; i < D.nvols; i++) {
		const struct rename *r = ren_for(D.vols[i].info.vol_id);
		int l = strlen(D.vols[i].info.name) + (r ? (int)strlen(r->to) + 2 : 0);

		w = imax(w, l);
	}
	return imin(w, 28);
}

static void draw_bar(int y)
{
	int x0 = 2, w = scr_cols - 4, i, total = D.info.total_lebs;
	long long cum = 0;
	int sx = -1, ex = -1;

	if (w < 10 || total <= 0)
		return;
	scr_fill(y, x0, w, st(S_DIM), '.');
	for (i = 0; i <= D.nvols; i++) {
		int lebs, mapped, a, b, style, k;

		if (i < D.nvols) {
			lebs = D.vols[i].info.rsvd_lebs;
			mapped = D.vols[i].mapped < 0 ? lebs : D.vols[i].mapped;
			if (D.vols[i].info.type == UBI_STATIC_VOLUME)
				mapped = (D.vols[i].info.data_bytes +
					  D.vols[i].info.leb_size - 1) /
					 imax(1, D.vols[i].info.leb_size);
		} else { /* UBI internal: layout volume, reserves */
			lebs = total - D.info.avail_lebs - (int)sum_rsvd();
			mapped = lebs;
		}
		a = x0 + (int)(cum * w / total);
		cum += lebs;
		b = x0 + (int)(cum * w / total);
		if (lebs > 0 && b == a)
			b = a + 1;
		if (b > x0 + w)
			b = x0 + w;
		style = i < D.nvols ? S_MAP1 + i % 6 : S_DIM;
		for (k = a; k < b; k++) {
			int is_mapped = k - a < (int)((long long)mapped * (b - a) /
						      imax(1, lebs)) ||
					mapped >= lebs;

			if (term_mono)
				scr_fill(y, k, 1, i == sel ? A_REV : 0,
					 i == D.nvols ? '=' : is_mapped ? '#' : ':');
			else if (i == D.nvols)
				scr_fill(y, k, 1, A_REV, ' ');
			else if (is_mapped)
				scr_fill(y, k, 1, st(style), ' ');
			else
				scr_acs(y, k, st(style) & 0x0f0 ? (st(style) >> 4) : 0,
					ACS_CKBOARD);
		}
		if (i < D.nvols && b - a >= 3) {
			char id[8];

			snprintf(id, sizeof(id), "%d", D.vols[i].info.vol_id);
			if ((int)strlen(id) <= b - a)
				scr_put(y, a + (b - a - (int)strlen(id)) / 2,
					term_mono ? A_BOLD : st(style) | A_BOLD, id);
		}
		if (i == sel) {
			sx = a;
			ex = b;
		}
	}
	if (sel == D.nvols) { /* free space is what is left on the right */
		sx = x0 + (int)(cum * w / total);
		ex = x0 + w;
	}
	if (sx >= 0 && ex > sx) {
		char label[UBI_VOL_NAME_MAX + 3];
		const char *name = cur_vol() ? cur_vol()->info.name : "free";

		if (ex - sx == 1) {
			scr_put(y + 1, sx, A_BOLD, "^");
		} else {
			scr_acs(y + 1, sx, A_BOLD, ACS_LLCORNER);
			scr_hline(y + 1, sx + 1, ex - sx - 2, A_BOLD);
			scr_acs(y + 1, ex - 1, A_BOLD, ACS_LRCORNER);
		}
		snprintf(label, sizeof(label), " %s ", name);
		{
			int lx = sx + (ex - sx - (int)strlen(label)) / 2;

			lx = imax(x0, imin(lx, x0 + w - (int)strlen(label)));
			scr_put(y + 1, lx, A_BOLD, label);
		}
	}
}

static void vol_flags(const struct vol *v, char *buf, size_t sz, int *style)
{
	*style = S_NORMAL;
	buf[0] = 0;
	if (removed_by_rename(v)) {
		snprintf(buf, sz, "REMOVED by staged rename");
		*style = S_ERR;
	} else if (v->upd_marker) {
		snprintf(buf, sz, "UPDATE INCOMPLETE");
		*style = S_ERR;
	} else if (v->info.corrupted) {
		snprintf(buf, sz, "CORRUPTED");
		*style = S_ERR;
	} else if (v->use[0]) {
		snprintf(buf, sz, "%s", v->use);
	}
}

static void main_draw(void *ctx)
{
	const struct menu_item *menu[16];
	char a[32], b[32], c[32], line[512];
	int y, i, nw, table_y, table_h, nmenu, menu_lines, info_y, x;
	int lw = 0, per_line;

	(void)ctx;
	if (!have_dev)
		return;
	snprintf(line, sizeof(line), "UBI device /dev/ubi%d on mtd%d \"%s\"", D.num,
		 D.info.mtd_num, D.have_mtd ? D.mtd.name : "?");
	scr_put(0, imax(0, (scr_cols - (int)strlen(line)) / 2), st(S_TITLE), line);
	if (D.have_mtd)
		snprintf(line, sizeof(line), "%s, %s, PEB %s, LEB %d bytes, min. I/O %d",
			 D.mtd.type_str, fmt_size(D.mtd.size, a, sizeof(a)),
			 fmt_size(D.mtd.eb_size, b, sizeof(b)), D.info.leb_size,
			 D.info.min_io_size);
	else
		snprintf(line, sizeof(line), "LEB %d bytes, min. I/O %d",
			 D.info.leb_size, D.info.min_io_size);
	scr_put(1, imax(0, (scr_cols - (int)strlen(line)) / 2), 0, line);
	snprintf(line, sizeof(line), "Good PEBs %d (bad %d): volumes %lld, free %d, "
		 "UBI internal %lld; max EC %lld", D.info.total_lebs, D.info.bad_count,
		 sum_rsvd(), D.info.avail_lebs,
		 D.info.total_lebs - D.info.avail_lebs - sum_rsvd(), D.info.max_ec);
	scr_put(2, imax(0, (scr_cols - (int)strlen(line)) / 2), 0, line);
	y = 3;
	if (D.ro_mode) {
		const char *w = "READ-ONLY MODE: UBI stopped writing after an error";

		scr_put(y, imax(0, (scr_cols - (int)strlen(w)) / 2), st(S_ERR), w);
	} else if (geteuid()) {
		const char *w = "Not running as root: changes will fail";

		scr_put(y, imax(0, (scr_cols - (int)strlen(w)) / 2), st(S_WARN), w);
	}
	draw_bar(4);

	/* bottom up: help line, menu, info box */
	nmenu = build_menu(menu);
	for (i = 0; i < nmenu; i++)
		lw = imax(lw, strlen(menu[i]->label));
	per_line = imax(1, (scr_cols - 2) / (lw + 6));
	menu_lines = (nmenu + per_line - 1) / per_line;
	info_y = scr_rows - 1 - menu_lines - 1 - 4;
	table_y = 7;
	table_h = info_y - table_y - 1;
	nw = name_width();

	/* table */
	snprintf(line, sizeof(line), "    %3s  %-*s %-7s %6s %9s %9s  %s", "ID", nw,
		 "Name", "Type", "LEBs", "Size", "Used", "Flags");
	scr_putn(table_y, 1, st(S_HEADER), line, scr_cols - 2);
	if (table_h > 0) {
		int rows = D.nvols + 1;

		if (sel < top)
			top = sel;
		if (sel >= top + table_h)
			top = sel - table_h + 1;
		if (top > imax(0, rows - table_h))
			top = imax(0, rows - table_h);
		for (i = 0; i < table_h && top + i < rows; i++) {
			int r = top + i, ry = table_y + 1 + i, fstyle;
			uint16_t base = r == sel ? A_REV : 0;
			char name[300], flags[200];

			if (r < D.nvols) {
				const struct vol *v = &D.vols[r];
				const struct rename *rn = ren_for(v->info.vol_id);
				long long used = v->info.type == UBI_STATIC_VOLUME ?
							 v->info.data_bytes :
						 v->mapped >= 0 ?
							 (long long)v->mapped * v->info.leb_size :
							 -1;

				vol_flags(v, flags, sizeof(flags), &fstyle);
				if (rn)
					snprintf(name, sizeof(name), "%s->%s", v->info.name, rn->to);
				else
					snprintf(name, sizeof(name), "%s", v->info.name);
				snprintf(line, sizeof(line), "%s %3d  %-*.*s %-7s %6d %9s %9s  ",
					 r == sel ? ">>" : "  ", v->info.vol_id, nw, nw, name,
					 v->info.type == UBI_STATIC_VOLUME ? "static" : "dynamic",
					 v->info.rsvd_lebs,
					 fmt_size(v->info.rsvd_bytes, a, sizeof(a)),
					 used < 0 ? "?" : fmt_size(used, b, sizeof(b)));
				x = 1 + (int)strlen(line);
				scr_putn(ry, 1, base | (removed_by_rename(v) ? st(S_ERR) : 0),
					 line, scr_cols - 2);
				if (rn)
					scr_putn(ry, 9, base | st(S_STAGED), name,
						 imin(nw, (int)strlen(name)));
				scr_putn(ry, x, base | st(fstyle), flags,
					 imax(0, scr_cols - 1 - x));
			} else {
				snprintf(line, sizeof(line), "%s %3s  %-*s %-7s %6d %9s",
					 r == sel ? ">>" : "  ", "", nw, "Free space", "",
					 D.info.avail_lebs,
					 fmt_size(D.info.avail_bytes, a, sizeof(a)));
				scr_putn(ry, 1, base | st(S_DIM), line, scr_cols - 2);
			}
		}
	}

	/* info box */
	if (info_y > table_y) {
		const struct vol *v = cur_vol();
		char l1[512], l2[512];

		scr_box(info_y, 1, 4, scr_cols - 2, st(S_BORDER));
		if (v) {
			char node[64];

			vol_node(D.num, v->info.vol_id, node, sizeof(node));
			snprintf(l1, sizeof(l1), "%s \"%s\": %s, %d LEBs = %s B (LEB %d B)",
				 node, v->info.name,
				 v->info.type == UBI_STATIC_VOLUME ? "static" : "dynamic",
				 v->info.rsvd_lebs, fmt_num(v->info.rsvd_bytes, a, sizeof(a)),
				 v->info.leb_size);
			if (v->info.type == UBI_STATIC_VOLUME)
				snprintf(l2, sizeof(l2), "data %s B", fmt_num(v->info.data_bytes,
					 b, sizeof(b)));
			else if (v->mapped >= 0)
				snprintf(l2, sizeof(l2), "mapped %d of %d LEBs", v->mapped,
					 v->info.rsvd_lebs);
			else
				snprintf(l2, sizeof(l2), "mapped LEBs unknown");
			snprintf(l2 + strlen(l2), sizeof(l2) - strlen(l2), ", alignment %d; %s",
				 v->info.alignment, v->use[0] ? v->use : "not mounted");
			if (ren_for(v->info.vol_id))
				snprintf(l2 + strlen(l2), sizeof(l2) - strlen(l2),
					 "; staged name \"%s\"", ren_for(v->info.vol_id)->to);
		} else {
			snprintf(l1, sizeof(l1), "%d LEBs = %s B free for new volumes or "
				 "for growing existing ones", D.info.avail_lebs,
				 fmt_num(D.info.avail_bytes, a, sizeof(a)));
			snprintf(l2, sizeof(l2), "volumes %d of %d, next free ID %d",
				 D.nvols, D.info.max_vol_count, next_free_id());
		}
		if (nren)
			snprintf(l2 + strlen(l2), sizeof(l2) - strlen(l2),
				 "  |  %d staged rename%s: a = apply, u = undo", nren,
				 nren > 1 ? "s" : "");
		scr_putn(info_y + 1, 3, 0, l1, scr_cols - 6);
		scr_putn(info_y + 2, 3, nren ? st(S_STAGED) : 0, l2, scr_cols - 6);
		(void)c;
	}

	/* menu */
	if (menu_sel >= nmenu)
		menu_sel = nmenu - 1;
	for (i = 0; i < nmenu; i++) {
		int row = i / per_line, col = i % per_line;
		int n_in_row = imin(per_line, nmenu - row * per_line);
		int mx = (scr_cols - n_in_row * (lw + 6)) / 2 + col * (lw + 6);
		int my = scr_rows - 1 - menu_lines + row - 1 + 1;
		int s = i == menu_sel;
		char item[40];
		const char *hp;

		my = scr_rows - 2 - (menu_lines - 1) + row;
		snprintf(item, sizeof(item), "[ %*s%-*s ]",
			 (lw - (int)strlen(menu[i]->label)) / 2, "",
			 lw - (lw - (int)strlen(menu[i]->label)) / 2, menu[i]->label);
		scr_put(my, mx, s ? st(S_SEL) : 0, item);
		hp = strchr(menu[i]->label, menu[i]->hot);
		if (!hp)
			hp = strchr(menu[i]->label, menu[i]->hot ^ 0x20);
		if (hp)
			scr_fill(my, mx + 2 + (lw - (int)strlen(menu[i]->label)) / 2 +
					 (int)(hp - menu[i]->label),
				 1, st(s ? S_HOT_SEL : S_HOT), *hp);
	}

	/* help / status line */
	if (status[0])
		scr_putn(scr_rows - 1, 1, st(status_style), status, scr_cols - 2);
	else if (nmenu) {
		const char *help = menu[menu_sel]->help;

		if (menu[menu_sel]->act == ACT_INFO && !cur_vol())
			help = "Details of the UBI device and its flash (ubinfo, mtdinfo)";
		scr_put(scr_rows - 1, imax(0, (scr_cols - (int)strlen(help)) / 2),
			st(S_DIM), help);
	}
}

/* ---- info views ---- */

static void kv(struct doc *d, const char *k, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
static void kv(struct doc *d, const char *k, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	doc_style(d, S_TITLE);
	doc_printf(d, "  %-20s", k);
	doc_style(d, S_NORMAL);
	doc_printf(d, "%s\n", buf);
}

static void vol_info_build(struct doc *d, int width, void *ctx)
{
	const struct vol *v = ctx;
	char node[64], a[32], b[32];
	int fd, l, per;

	vol_node(D.num, v->info.vol_id, node, sizeof(node));
	kv(d, "Volume ID", "%d", v->info.vol_id);
	kv(d, "Name", "%s", v->info.name);
	if (ren_for(v->info.vol_id))
		kv(d, "Staged new name", "%s", ren_for(v->info.vol_id)->to);
	kv(d, "Character device", "%s (%d:%d)", node, v->info.major, v->info.minor);
	kv(d, "Type", "%s", v->info.type == UBI_STATIC_VOLUME ? "static" : "dynamic");
	kv(d, "Alignment", "%d (data pad %d bytes per LEB)", v->info.alignment,
	   D.info.leb_size - v->info.leb_size);
	kv(d, "LEB size", "%d bytes", v->info.leb_size);
	kv(d, "Reserved", "%d LEBs, %s bytes (%s)", v->info.rsvd_lebs,
	   fmt_num(v->info.rsvd_bytes, a, sizeof(a)),
	   fmt_size(v->info.rsvd_bytes, b, sizeof(b)));
	kv(d, "Data bytes", "%s%s", fmt_num(v->info.data_bytes, a, sizeof(a)),
	   v->info.type == UBI_STATIC_VOLUME ? "" : " (dynamic: always the reserved size)");
	if (v->mapped >= 0)
		kv(d, "Mapped LEBs", "%d of %d (unmapped LEBs read as 0xFF)", v->mapped,
		   v->info.rsvd_lebs);
	kv(d, "Corrupted", "%s", v->info.corrupted ? "YES" : "no");
	kv(d, "Update marker", "%d%s", v->upd_marker,
	   v->upd_marker ? " (an update was interrupted: write or wipe the volume)" : "");
	if (v->block)
		kv(d, "Block device", "/dev/ubiblock%d_%d", D.num, v->info.vol_id);
	kv(d, "In use", "%s", v->use[0] ? v->use : "no mount found");
	if (removed_by_rename(v)) {
		doc_style(d, S_ERR);
		doc_printf(d, "\n  Applying the staged renames removes this volume.\n");
		doc_style(d, S_NORMAL);
	}

	doc_printf(d, "\n");
	doc_style(d, S_TITLE);
	doc_printf(d, "LEB map");
	doc_style(d, S_NORMAL);
	doc_printf(d, "  (# mapped: holds data, . unmapped)\n");
	fd = open(node, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		doc_printf(d, "  cannot open %s: %s\n", node, strerror(errno));
		return;
	}
	per = imax(16, (width - 8) / 16 * 16);
	for (l = 0; l < v->info.rsvd_lebs; l++) {
		int m = ubi_is_mapped(fd, l);

		if (l % per == 0)
			doc_printf(d, "%s%6d ", l ? "\n" : "", l);
		doc_style(d, m > 0 ? S_MAP1 : S_DIM);
		doc_printf(d, "%c", m < 0 ? '?' : m ? '#' : '.');
		doc_style(d, S_NORMAL);
	}
	doc_printf(d, "\n");
	close(fd);
}

static void dev_info_build(struct doc *d, int width, void *ctx)
{
	struct ubi_info ui;
	char a[32], b[32];

	(void)width;
	(void)ctx;
	kv(d, "UBI device", "ubi%d (/dev/ubi%d, %d:%d)", D.num, D.num, D.info.major,
	   D.info.minor);
	if (lubi && !ubi_get_info(lubi, &ui))
		kv(d, "UBI version", "%d, %d device(s) attached", ui.version,
		   ui.dev_count);
	if (D.have_mtd) {
		kv(d, "MTD device", "mtd%d \"%s\" (/dev/mtd%d), %s%s", D.mtd.mtd_num,
		   D.mtd.name, D.mtd.mtd_num, D.mtd.type_str,
		   D.mtd.writable ? "" : ", read-only");
		kv(d, "Flash size", "%s bytes (%s), %d eraseblocks of %d bytes",
		   fmt_num(D.mtd.size, a, sizeof(a)), fmt_size(D.mtd.size, b, sizeof(b)),
		   D.mtd.eb_cnt, D.mtd.eb_size);
		kv(d, "Min. I/O / sub-page", "%d / %d bytes; OOB %d bytes", D.mtd.min_io_size,
		   D.mtd.subpage_size, D.mtd.oob_size);
		kv(d, "Bad blocks possible", "%s", D.mtd.bb_allowed ? "yes" : "no");
	}
	kv(d, "LEB size", "%d bytes", D.info.leb_size);
	kv(d, "Good PEBs", "%d (bad %d)", D.info.total_lebs, D.info.bad_count);
	kv(d, "Reserved for bad", "%d PEBs", D.info.bad_rsvd);
	kv(d, "Volumes", "%d of %d, reserving %lld LEBs", D.nvols, D.info.max_vol_count,
	   sum_rsvd());
	kv(d, "Available", "%d LEBs, %s bytes (%s)", D.info.avail_lebs,
	   fmt_num(D.info.avail_bytes, a, sizeof(a)),
	   fmt_size(D.info.avail_bytes, b, sizeof(b)));
	kv(d, "UBI internal", "%lld PEBs (layout volume, wear-leveling and bad-block "
	   "reserves)", D.info.total_lebs - D.info.avail_lebs - sum_rsvd());
	kv(d, "Max erase counter", "%lld", D.info.max_ec);
	kv(d, "Background thread", "%s",
	   sysfs_int("/sys/class/ubi/ubi%d/bgt_enabled", D.num, 0, 0) ? "enabled" :
								      "disabled");
	kv(d, "Read-only mode", "%s", D.ro_mode ? "YES (after an error)" : "no");
	if (D.have_mtd) {
		struct ubigen_info g;

		ubigen_info_init(&g, D.mtd.eb_size, D.mtd.min_io_size,
				 D.mtd.subpage_size, 0, 1, 0);
		doc_printf(d, "\n");
		doc_style(d, S_TITLE);
		doc_printf(d, "Defaults for this flash (mtdinfo -u)\n");
		doc_style(d, S_NORMAL);
		kv(d, "VID header offset", "%d", g.vid_hdr_offs);
		kv(d, "Data offset", "%d", g.data_offs);
		kv(d, "LEB size", "%d", g.leb_size);
		kv(d, "Max volumes", "%d", g.max_volumes);
	}
}

static void act_info(void)
{
	struct vol *v = cur_vol();
	char title[200];

	if (v) {
		snprintf(title, sizeof(title), "Volume %d \"%s\" (ubinfo)", v->info.vol_id,
			 v->info.name);
		ui_view(title, vol_info_build, v);
	} else {
		snprintf(title, sizeof(title), "Device ubi%d (ubinfo, mtdinfo)", D.num);
		ui_view(title, dev_info_build, NULL);
	}
}

/* ---- create / resize ---- */

static const char *const vol_types[] = { "dynamic", "static", NULL };

static int vol_leb_for(int alignment)
{
	return D.info.leb_size - D.info.leb_size % alignment;
}

struct newctx {
	long long bytes, lebs;
	int alignment, vol_id;
};

static int new_check(struct field *f, int n, char *note, size_t sz, void *vctx)
{
	struct newctx *c = vctx;
	char err[200], a[32], b[32];
	long long lebs_unit;
	char *end;
	int leb, i;

	(void)n;
	f[5].hidden = f[2].choice != 1;
	if (!f[0].buf[0]) {
		snprintf(note, sz, "Enter a name.");
		return 1;
	}
	if (vol_find_name(&D, f[0].buf) >= 0) {
		snprintf(note, sz, "Volume \"%s\" already exists.", f[0].buf);
		return 1;
	}
	for (i = 0; i < nren; i++) {
		if (!strcmp(ren[i].to, f[0].buf)) {
			snprintf(note, sz, "\"%s\" is a staged new name.", f[0].buf);
			return 1;
		}
	}
	c->alignment = strtol(f[4].buf, &end, 0);
	if (*end || c->alignment < 1 || c->alignment > D.info.leb_size) {
		snprintf(note, sz, "Alignment: 1 to %d.", D.info.leb_size);
		return 1;
	}
	if (!strcasecmp(f[3].buf, "auto") || !f[3].buf[0]) {
		c->vol_id = UBI_VOL_NUM_AUTO;
	} else {
		c->vol_id = strtol(f[3].buf, &end, 0);
		if (*end || c->vol_id < 0 || c->vol_id >= D.info.max_vol_count) {
			snprintf(note, sz, "Volume ID: auto or 0 to %d.",
				 D.info.max_vol_count - 1);
			return 1;
		}
		for (i = 0; i < D.nvols; i++) {
			if (D.vols[i].info.vol_id == c->vol_id) {
				snprintf(note, sz, "Volume ID %d is in use.", c->vol_id);
				return 1;
			}
		}
	}
	leb = vol_leb_for(c->alignment);
	c->bytes = parse_size(f[1].buf, leb, (long long)D.info.avail_lebs * leb,
			      &lebs_unit, err, sizeof(err));
	if (c->bytes < 0) {
		snprintf(note, sz, "%s", err);
		return 1;
	}
	c->lebs = (c->bytes + leb - 1) / leb;
	if (c->lebs < 1) {
		snprintf(note, sz, "The size must be at least 1 byte.");
		return 1;
	}
	if (c->lebs > D.info.avail_lebs) {
		snprintf(note, sz, "%lld LEBs needed, only %d free.", c->lebs,
			 D.info.avail_lebs);
		return 1;
	}
	snprintf(note, sz, "= %lld LEBs of %d bytes = %s bytes (%s); %lld LEBs stay "
		 "free.", c->lebs, leb, fmt_num(c->lebs * leb, a, sizeof(a)),
		 fmt_size(c->lebs * leb, b, sizeof(b)), D.info.avail_lebs - c->lebs);
	return 0;
}

static void act_new(void)
{
	struct field f[6] = {
		{ .type = F_TEXT, .label = "Name:", .maxlen = UBI_VOL_NAME_MAX,
		  .help = "Volume name, up to 127 characters (ubimkvol -N)" },
		{ .type = F_TEXT, .label = "Size:", .maxlen = 30,
		  .help = "e.g. 12MiB, 1.5M, 96LEB, 4096 (bytes) or max (ubimkvol -s/-S/-m)" },
		{ .type = F_CHOICE, .label = "Type:", .choices = vol_types,
		  .help = "dynamic: read/write (UBIFS, raw data); static: read-only, CRC-checked" },
		{ .type = F_TEXT, .label = "Volume ID:", .maxlen = 5,
		  .help = "auto, or a free ID (ubimkvol -n)" },
		{ .type = F_TEXT, .label = "Alignment:", .maxlen = 7,
		  .help = "LEB alignment, usually 1 (ubimkvol -a)" },
		{ .type = F_CHECK, .label = "Skip CRC check:",
		  .help = "Do not check the static volume CRC when it is opened (ubimkvol -k)" },
	};
	struct ubi_mkvol_request req;
	struct newctx c;
	char node[64];

	if (D.info.avail_lebs <= 0) {
		ui_error("New volume", "There are no free LEBs. Shrink or delete a volume "
			 "first.");
		return;
	}
	snprintf(f[1].buf, sizeof(f[1].buf), "max");
	snprintf(f[3].buf, sizeof(f[3].buf), "auto");
	snprintf(f[4].buf, sizeof(f[4].buf), "1");
	if (!ui_form("New volume (ubimkvol)", NULL, f, 6, new_check, &c))
		return;
	memset(&req, 0, sizeof(req));
	req.vol_id = c.vol_id;
	req.alignment = c.alignment;
	req.bytes = c.lebs * vol_leb_for(c.alignment);
	req.vol_type = f[2].choice ? UBI_STATIC_VOLUME : UBI_DYNAMIC_VOLUME;
	req.name = f[0].buf;
	req.flags = f[2].choice && f[5].checked ? UBI_VOL_SKIP_CRC_CHECK_FLG : 0;
	dev_node(D.num, node, sizeof(node));
	op_begin();
	if (ubi_mkvol(lubi, node, &req)) {
		op_fail("New volume", errno, "Cannot create volume \"%s\".", f[0].buf);
		reload();
		return;
	}
	reload();
	select_vol(req.vol_id);
	set_status(S_OK, "Created volume %d \"%s\", %lld LEBs.", req.vol_id, f[0].buf,
		   c.lebs);
}

struct rsctx {
	const struct vol *v;
	long long lebs;
	long long end_lebs; /* LEBs up to the last one holding data */
};

static int resize_check(struct field *f, int n, char *note, size_t sz, void *vctx)
{
	struct rsctx *c = vctx;
	const struct vol *v = c->v;
	long long bytes, lebs_unit, max = v->info.rsvd_lebs + D.info.avail_lebs;
	char err[200], a[32];

	(void)n;
	bytes = parse_size(f[0].buf, v->info.leb_size, max * v->info.leb_size,
			   &lebs_unit, err, sizeof(err));
	if (bytes < 0) {
		snprintf(note, sz, "%s", err);
		return 1;
	}
	c->lebs = (bytes + v->info.leb_size - 1) / v->info.leb_size;
	if (c->lebs < 1) {
		snprintf(note, sz, "At least 1 LEB.");
		return 1;
	}
	if (c->lebs > max) {
		snprintf(note, sz, "At most %lld LEBs (%d now + %d free).", max,
			 v->info.rsvd_lebs, D.info.avail_lebs);
		return 1;
	}
	if (v->info.type == UBI_STATIC_VOLUME &&
	    c->lebs * v->info.leb_size < v->info.data_bytes) {
		snprintf(note, sz, "A static volume cannot shrink below its data "
			 "(%lld bytes).", v->info.data_bytes);
		return 1;
	}
	snprintf(note, sz, "%d -> %lld LEBs (%+lld) = %s; %lld LEBs stay free.%s",
		 v->info.rsvd_lebs, c->lebs, c->lebs - v->info.rsvd_lebs,
		 fmt_size(c->lebs * v->info.leb_size, a, sizeof(a)),
		 max - c->lebs,
		 c->end_lebs > c->lebs ? " Data in the cut-off LEBs is LOST." : "");
	return 0;
}

static void act_resize(void)
{
	struct vol *v = cur_vol();
	struct field f[1] = { { .type = F_TEXT, .label = "New size:", .maxlen = 30,
				.help = "e.g. 20MiB, 120LEB or max (all free space)" } };
	struct rsctx c = { .v = v };
	char text[512], node[64];

	c.end_lebs = (content_end(v) + v->info.leb_size - 1) / v->info.leb_size;

	snprintf(f[0].buf, sizeof(f[0].buf), "%d LEB", v->info.rsvd_lebs);
	snprintf(text, sizeof(text), "Volume %d \"%s\" has %d LEBs. Growing uses free "
		 "LEBs; shrinking a dynamic volume drops the data at its end.",
		 v->info.vol_id, v->info.name, v->info.rsvd_lebs);
	if (!ui_form("Resize volume (ubirsvol)", text, f, 1, resize_check, &c))
		return;
	if (c.lebs == v->info.rsvd_lebs)
		return;
	if (c.lebs < v->info.rsvd_lebs) {
		snprintf(text, sizeof(text), "Shrink volume %d \"%s\" from %d to %lld "
			 "LEBs?%s", v->info.vol_id, v->info.name, v->info.rsvd_lebs,
			 c.lebs, c.end_lebs > c.lebs ?
				 " LEBs beyond the new end hold data, which is lost." : "");
		if (c.end_lebs > c.lebs ? !ui_confirm_yes("Shrink volume", text) :
					  !ui_yesno("Shrink volume", text, 0))
			return;
	}
	dev_node(D.num, node, sizeof(node));
	op_begin();
	if (ubi_rsvol(lubi, node, v->info.vol_id, c.lebs * v->info.leb_size)) {
		op_fail("Resize volume", errno, "Cannot resize volume %d \"%s\".",
			v->info.vol_id, v->info.name);
		reload();
		return;
	}
	set_status(S_OK, "Volume %d \"%s\" now has %lld LEBs.", v->info.vol_id,
		   v->info.name, c.lebs);
	reload();
}

static void act_delete(void)
{
	struct vol *v = cur_vol();
	char text[1024], node[64], a[32];
	int id = v->info.vol_id;

	snprintf(text, sizeof(text), "Delete volume %d \"%s\" (%d LEBs, %s)? Its "
		 "data is lost.%s%s%s", id, v->info.name, v->info.rsvd_lebs,
		 fmt_size(v->info.rsvd_bytes, a, sizeof(a)),
		 v->use[0] ? "\n\nIn use: " : "", v->use,
		 ren_for(id) ? "\n\nIts staged rename is dropped." : "");
	if (!ui_confirm_yes("Delete volume (ubirmvol)", text))
		return;
	dev_node(D.num, node, sizeof(node));
	op_begin();
	if (ubi_rmvol(lubi, node, id)) {
		op_fail("Delete volume", errno, "Cannot delete volume %d \"%s\".", id,
			v->info.name);
		reload();
		return;
	}
	set_status(S_OK, "Deleted volume %d \"%s\".", id, v->info.name);
	ren_drop(id);
	reload();
}

/* ---- renames ---- */

struct renctx {
	const struct vol *v;
};

static int rename_check(struct field *f, int n, char *note, size_t sz, void *vctx)
{
	struct renctx *c = vctx;
	const char *to = f[0].buf;
	int i, victim = -1;

	(void)n;
	if (!*to) {
		snprintf(note, sz, "Enter a name.");
		return 1;
	}
	if (!strcmp(to, c->v->info.name)) {
		snprintf(note, sz, ren_for(c->v->info.vol_id) ?
				      "Back to the current name: the staged rename is dropped." :
				      "The current name.");
		return 0;
	}
	for (i = 0; i < nren; i++) {
		if (ren[i].vol_id != c->v->info.vol_id && !strcmp(ren[i].to, to)) {
			snprintf(note, sz, "Volume %d is already staged to become \"%s\".",
				 ren[i].vol_id, to);
			return 1;
		}
	}
	if (!ren_for(c->v->info.vol_id) && nren >= UBI_MAX_RNVOL) {
		snprintf(note, sz, "At most %d renames at once.", UBI_MAX_RNVOL);
		return 1;
	}
	i = vol_find_name(&D, to);
	if (i >= 0 && !ren_for(D.vols[i].info.vol_id))
		victim = i;
	if (victim >= 0)
		snprintf(note, sz, "Volume %d \"%s\" will be REMOVED when the renames "
			 "are applied, unless it is renamed too (UBI replaces it "
			 "atomically).", D.vols[victim].info.vol_id, to);
	else
		snprintf(note, sz, "Staged; applied with Apply (a) together with the "
			 "other staged renames.");
	return 0;
}

static void act_rename(void)
{
	struct vol *v = cur_vol();
	const struct rename *r = ren_for(v->info.vol_id);
	struct field f[1] = { { .type = F_TEXT, .label = "New name:",
				.maxlen = UBI_VOL_NAME_MAX,
				.help = "Renames are staged and applied together, atomically" } };
	struct renctx c = { v };
	char text[400];
	int i;

	snprintf(f[0].buf, sizeof(f[0].buf), "%s", r ? r->to : v->info.name);
	snprintf(text, sizeof(text), "Volume %d \"%s\". Swapping names, e.g. "
		 "kernel <-> kernel_new, works by staging both renames.",
		 v->info.vol_id, v->info.name);
	if (!ui_form("Rename volume (ubirename)", text, f, 1, rename_check, &c))
		return;
	ren_drop(v->info.vol_id);
	if (strcmp(f[0].buf, v->info.name)) {
		i = nren++;
		ren[i].vol_id = v->info.vol_id;
		snprintf(ren[i].from, sizeof(ren[i].from), "%s", v->info.name);
		snprintf(ren[i].to, sizeof(ren[i].to), "%.127s", f[0].buf);
		set_status(S_STAGED, "Staged \"%s\" -> \"%s\". a: apply all %d, u: undo.",
			   v->info.name, f[0].buf, nren);
	}
}

static void act_apply(void)
{
	struct ubi_rnvol_req req;
	char text[4096], node[64];
	int i, n = 0, removals = 0;

	n = snprintf(text, sizeof(text), "Apply in one atomic operation:\n\n");
	for (i = 0; i < nren; i++)
		n += snprintf(text + n, sizeof(text) - n, "  %d: \"%s\" -> \"%s\"\n",
			      ren[i].vol_id, ren[i].from, ren[i].to);
	for (i = 0; i < D.nvols; i++) {
		if (removed_by_rename(&D.vols[i])) {
			n += snprintf(text + n, sizeof(text) - n,
				      "  %d: \"%s\" is REMOVED (its name is taken over)\n",
				      D.vols[i].info.vol_id, D.vols[i].info.name);
			removals++;
		}
	}
	if (removals ? !ui_confirm_yes("Apply renames (ubirename)", text) :
		       !ui_yesno("Apply renames (ubirename)", text, 1))
		return;
	memset(&req, 0, sizeof(req));
	req.count = nren;
	for (i = 0; i < nren; i++) {
		req.ents[i].vol_id = ren[i].vol_id;
		req.ents[i].name_len = strlen(ren[i].to);
		memcpy(req.ents[i].name, ren[i].to, req.ents[i].name_len + 1);
	}
	dev_node(D.num, node, sizeof(node));
	op_begin();
	if (ubi_rnvols(lubi, node, &req)) {
		op_fail("Apply renames", errno, "The renames were not applied; nothing "
			"changed.");
		reload();
		return;
	}
	set_status(S_OK, "Applied %d rename%s%s.", nren, nren > 1 ? "s" : "",
		   removals ? ", removed the replaced volume(s)" : "");
	nren = 0;
	reload();
}

static void act_swap(void)
{
	struct vol *v = cur_vol();
	struct list_item it[MAX_VOLS];
	char labels[MAX_VOLS][160];
	int idx[MAX_VOLS], n = 0, i, k;

	for (i = 0; i < D.nvols; i++) {
		if (&D.vols[i] == v)
			continue;
		snprintf(labels[n], sizeof(labels[n]), "%3d  %s", D.vols[i].info.vol_id,
			 D.vols[i].info.name);
		it[n] = (struct list_item){ labels[n], NULL, 0, 0 };
		idx[n++] = i;
	}
	if (!n) {
		ui_error("Swap names", "There is no other volume.");
		return;
	}
	if (nren + 2 > UBI_MAX_RNVOL) {
		ui_error("Swap names", "Too many staged renames.");
		return;
	}
	k = ui_list("Swap names with", "Stage two renames that exchange the names; "
		    "Apply (a) performs them atomically.", it, n, 0);
	if (k < 0)
		return;
	{
		struct vol *w = &D.vols[idx[k]];

		ren_drop(v->info.vol_id);
		ren_drop(w->info.vol_id);
		ren[nren].vol_id = v->info.vol_id;
		snprintf(ren[nren].from, sizeof(ren[nren].from), "%s", v->info.name);
		snprintf(ren[nren].to, sizeof(ren[nren].to), "%s", w->info.name);
		nren++;
		ren[nren].vol_id = w->info.vol_id;
		snprintf(ren[nren].from, sizeof(ren[nren].from), "%s", w->info.name);
		snprintf(ren[nren].to, sizeof(ren[nren].to), "%s", v->info.name);
		nren++;
		set_status(S_STAGED, "Staged swap of \"%s\" and \"%s\". a: apply, u: undo.",
			   v->info.name, w->info.name);
	}
}

/* ---- data transfer ---- */

static int write_all(int fd, const void *buf, long long n)
{
	const char *p = buf;

	while (n > 0) {
		ssize_t w = write(fd, p, n);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		p += w;
		n -= w;
	}
	return 0;
}

/* MD5 and UBI CRC32 (ubicrc32) of the first len bytes of a stream. Returns
 * the bytes read, -1 on error, -2 when cancelled. */
static long long hash_stream(int fd, struct src *s, long long len, char md5[33],
			     uint32_t *crc, const char *title, const char *what)
{
	struct progress p;
	struct md5 m;
	long long done = 0;
	char *buf = malloc(1 << 17);

	if (!buf)
		return -1;
	md5_init(&m);
	*crc = UBI_CRC32_INIT;
	prog_start(&p, title, len, 1);
	while (done < len) {
		long long want = len - done < (1 << 17) ? len - done : (1 << 17), n;

		n = s ? src_read(s, buf, want) : read(fd, buf, want);
		if (n < 0 && !s && errno == EINTR)
			continue;
		if (n < 0) {
			done = -1;
			break;
		}
		if (n == 0)
			break;
		md5_update(&m, buf, n);
		*crc = mtd_crc32(*crc, buf, n);
		done += n;
		if (prog_update(&p, done, "%s", what)) {
			done = -2;
			break;
		}
	}
	prog_end(&p);
	free(buf);
	md5_hex(&m, md5);
	return done;
}

/* Last mapped LEB + 1 (dynamic volumes), the data size for static ones. */
static long long content_end(const struct vol *v)
{
	char node[64];
	int fd, l;

	if (v->info.type == UBI_STATIC_VOLUME)
		return v->info.data_bytes;
	vol_node(D.num, v->info.vol_id, node, sizeof(node));
	fd = open(node, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return v->info.rsvd_bytes;
	for (l = v->info.rsvd_lebs - 1; l >= 0; l--) {
		int m = ubi_is_mapped(fd, l);

		if (m < 0) {
			l = v->info.rsvd_lebs - 1;
			break;
		}
		if (m)
			break;
	}
	close(fd);
	return (long long)(l + 1) * v->info.leb_size;
}

/* ubiupdatevol: writes size bytes of in to the volume, then optionally reads
 * them back. Reports the result. */
static int do_update(const struct vol *v, struct src *in, long long size,
		     int verify, const char *title, const char *what)
{
	char node[64], md5w[33], md5r[33], a[32];
	struct progress p;
	struct md5 m;
	long long done = 0;
	char *buf;
	int fd, err = 0, cancelled = 0, leb = v->info.leb_size;
	uint32_t crc;
	struct timespec t0, t1;

	vol_node(D.num, v->info.vol_id, node, sizeof(node));
	buf = malloc(leb);
	op_begin();
	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0 || !buf) {
		op_fail(title, errno, "Cannot open %s.", node);
		free(buf);
		return -1;
	}
	if (ubi_update_start(lubi, fd, size)) {
		op_fail(title, errno, "Cannot start the update of volume %d \"%s\".",
			v->info.vol_id, v->info.name);
		close(fd);
		free(buf);
		return -1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	md5_init(&m);
	prog_start(&p, title, size, 1);
	while (done < size) {
		long long want = size - done < leb ? size - done : leb;
		long long n = src_read(in, buf, want);

		if (n != want) {
			err = n < 0 ? errno : 0;
			if (n > 0)
				done += n;
			break;
		}
		md5_update(&m, buf, n);
		if (write_all(fd, buf, n)) {
			err = errno;
			break;
		}
		done += n;
		if (prog_update(&p, done, "%s", what)) {
			cancelled = 1;
			break;
		}
	}
	prog_end(&p);
	close(fd);
	free(buf);
	if (done < size) {
		reload();
		op_fail(title, err, "%s after %s of %s bytes%s. Volume %d \"%s\" is left "
			"with an incomplete update (update marker set) and is unusable "
			"until it is written again or wiped.",
			cancelled ? "Cancelled" : err ? "Writing failed" : "The input ended",
			fmt_num(done, md5r, sizeof(md5r)), fmt_num(size, a, sizeof(a)),
			cancelled || err ? "" : " (input too short)", v->info.vol_id,
			v->info.name);
		return -1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	md5_hex(&m, md5w);
	if (verify) {
		long long r;

		fd = open(node, O_RDONLY | O_CLOEXEC);
		r = fd < 0 ? -1 : hash_stream(fd, NULL, size, md5r, &crc, "Verifying",
					      "Reading the volume back");
		if (fd >= 0)
			close(fd);
		if (r == -2) {
			set_status(S_WARN, "Wrote %s bytes (MD5 %s); verification "
				   "cancelled.", fmt_num(size, a, sizeof(a)), md5w);
			return 0;
		}
		if (r != size || strcmp(md5r, md5w)) {
			ui_error(title, "Wrote %s bytes, but reading them back %s.\n\n"
				 "Written MD5:   %s\nRead back MD5: %s",
				 fmt_num(size, a, sizeof(a)),
				 r != size ? "failed" : "gives different data",
				 md5w, r == size ? md5r : "-");
			return -1;
		}
	}
	ui_msg(title, "Wrote %s bytes to volume %d \"%s\" in %.1f s.\n\nMD5 %s\n%s",
	       fmt_num(size, a, sizeof(a)), v->info.vol_id, v->info.name,
	       (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9, md5w,
	       verify ? "Read back and verified: identical." : "Not verified.");
	return 0;
}

static int write_check(struct field *f, int n, char *note, size_t sz, void *vctx)
{
	const struct vol *v = vctx;
	char err[200], a[32];
	long long lebs, x;

	(void)n;
	if (!f[0].buf[0]) {
		snprintf(note, sz, "Enter the source: a file, a device such as "
			 "/dev/ubi0_3, http://host:port/path, or - (stdin).");
		return 1;
	}
	x = parse_size(f[1].buf, v->info.leb_size, 0, &lebs, err, sizeof(err));
	if (x < 0) {
		snprintf(note, sz, "Skip: %s", err);
		return 1;
	}
	if (strcasecmp(f[2].buf, "auto")) {
		x = parse_size(f[2].buf, v->info.leb_size, v->info.rsvd_bytes, &lebs, err,
			       sizeof(err));
		if (x < 0) {
			snprintf(note, sz, "Size: %s", err);
			return 1;
		}
		if (x > v->info.rsvd_bytes) {
			snprintf(note, sz, "At most %lld bytes fit.", v->info.rsvd_bytes);
			return 1;
		}
	} else if (!strcmp(f[0].buf, "-")) {
		snprintf(note, sz, "Standard input has no known size: enter it.");
		return 1;
	}
	snprintf(note, sz, "Volume %d \"%s\" holds up to %s bytes. Its contents are "
		 "replaced.", v->info.vol_id, v->info.name,
		 fmt_num(v->info.rsvd_bytes, a, sizeof(a)));
	return 0;
}

static void act_write(void)
{
	struct vol *v = cur_vol();
	struct field f[4] = {
		{ .type = F_TEXT, .label = "Source:", .browse = 1,
		  .help = "File, device, http://host:port/path or - (stdin)" },
		{ .type = F_TEXT, .label = "Skip bytes:", .maxlen = 20,
		  .help = "Leading input bytes to skip (ubiupdatevol --skip)" },
		{ .type = F_TEXT, .label = "Size:", .maxlen = 20,
		  .help = "auto (from the input) or bytes to write (ubiupdatevol -s)" },
		{ .type = F_CHECK, .label = "Verify:", .checked = 1,
		  .help = "Read the volume back and compare the MD5" },
	};
	struct src in;
	char err[512], text[1024], a[32], *spec;
	long long skip, size, lebs;

	snprintf(f[1].buf, sizeof(f[1].buf), "0");
	snprintf(f[2].buf, sizeof(f[2].buf), "auto");
	if (!ui_form("Write image to volume (ubiupdatevol)", NULL, f, 4, write_check,
		     v))
		return;
	spec = f[0].buf;
	skip = parse_size(f[1].buf, v->info.leb_size, 0, &lebs, err, sizeof(err));
	if (src_open(&in, spec, skip, err, sizeof(err))) {
		ui_error("Write", "%s", err);
		return;
	}
	size = strcasecmp(f[2].buf, "auto") ?
		       parse_size(f[2].buf, v->info.leb_size, v->info.rsvd_bytes, &lebs,
				  err, sizeof(err)) :
		       in.size;
	if (size < 0) {
		ui_error("Write", "The size of %s is not known; enter it.", spec);
		src_close(&in);
		return;
	}
	if (size > v->info.rsvd_bytes) {
		ui_error("Write", "%s bytes do not fit volume %d \"%s\" (%s bytes). "
			 "Resize it first.", fmt_num(size, err, sizeof(err)),
			 v->info.vol_id, v->info.name,
			 fmt_num(v->info.rsvd_bytes, a, sizeof(a)));
		src_close(&in);
		return;
	}
	snprintf(text, sizeof(text), "Replace the contents of volume %d \"%s\" with "
		 "%s bytes from %s?%s%s", v->info.vol_id, v->info.name,
		 fmt_num(size, a, sizeof(a)), strcmp(spec, "-") ? spec : "standard input",
		 v->use[0] ? "\n\nIn use: " : "", v->use);
	if (!ui_confirm_yes("Write volume", text)) {
		src_close(&in);
		return;
	}
	if (!do_update(v, &in, size, f[3].checked, "Write volume", spec))
		set_status(S_OK, "Wrote %s bytes to volume %d \"%s\".",
			   fmt_num(size, a, sizeof(a)), v->info.vol_id, v->info.name);
	src_close(&in);
	reload();
}

static void act_wipe(void)
{
	struct vol *v = cur_vol();
	char text[400], node[64];
	int fd;

	snprintf(text, sizeof(text), "Wipe volume %d \"%s\"? All its data is erased "
		 "(ubiupdatevol -t); the volume itself stays.", v->info.vol_id,
		 v->info.name);
	if (!ui_confirm_yes("Wipe volume", text))
		return;
	vol_node(D.num, v->info.vol_id, node, sizeof(node));
	op_begin();
	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0 || ubi_update_start(lubi, fd, 0)) {
		op_fail("Wipe volume", errno, "Cannot wipe volume %d \"%s\".",
			v->info.vol_id, v->info.name);
	} else {
		set_status(S_OK, "Wiped volume %d \"%s\".", v->info.vol_id, v->info.name);
	}
	if (fd >= 0)
		close(fd);
	reload();
}

static const char *const backup_lens[] = { "whole volume",
					    "up to the last mapped LEB",
					    "strip trailing 0xFF bytes", NULL };

struct backupctx {
	const struct vol *v;
	long long end; /* content_end() */
};

static int backup_check(struct field *f, int n, char *note, size_t sz, void *vctx)
{
	struct backupctx *c = vctx;
	struct stat s;
	struct statvfs fs;
	char dir[300], *slash, a[32];
	long long need = f[1].choice ? c->end : c->v->info.data_bytes;

	(void)n;
	if (!f[0].buf[0]) {
		snprintf(note, sz, "Enter a file name.");
		return 1;
	}
	if (!stat(f[0].buf, &s) && !f[2].checked) {
		snprintf(note, sz, "%s exists: tick Overwrite.", f[0].buf);
		return 1;
	}
	snprintf(dir, sizeof(dir), "%s", f[0].buf);
	slash = strrchr(dir, '/');
	if (slash)
		*(slash == dir ? slash + 1 : slash) = 0;
	else
		snprintf(dir, sizeof(dir), ".");
	if (!statvfs(dir, &fs) && fs.f_blocks &&
	    (long long)fs.f_bavail * fs.f_bsize < need) {
		snprintf(note, sz, "Only %s free in %s.",
			 fmt_size((long long)fs.f_bavail * fs.f_bsize, a, sizeof(a)), dir);
		return 1;
	}
	snprintf(note, sz, "Up to %s bytes are written.%s", fmt_num(need, a, sizeof(a)),
		 !statvfs(dir, &fs) && !fs.f_blocks ?
			 " The directory is in RAM (ramfs, no size limit): mind the "
			 "free memory." : "");
	return 0;
}

static void act_backup(void)
{
	struct vol *v = cur_vol();
	struct backupctx c = { v, content_end(v) };
	struct field f[3] = {
		{ .type = F_TEXT, .label = "File:", .browse = 1,
		  .help = "Destination file" },
		{ .type = F_CHOICE, .label = "Length:", .choices = backup_lens,
		  .help = "Unmapped LEBs and erased flash read as 0xFF" },
		{ .type = F_CHECK, .label = "Overwrite:",
		  .help = "Replace an existing file" },
	};
	char node[64], md5[33], a[32];
	struct progress p;
	struct md5 m;
	long long len, done = 0, last_data = 0;
	char *buf;
	int in, out, cancelled = 0, err = 0;

	snprintf(f[0].buf, sizeof(f[0].buf), "/tmp/%s.img", v->info.name);
	if (v->info.type == UBI_STATIC_VOLUME)
		f[1].hidden = 1;
	if (!ui_form("Backup volume to a file", NULL, f, 3, backup_check, &c))
		return;
	len = f[1].choice ? c.end : v->info.data_bytes;
	vol_node(D.num, v->info.vol_id, node, sizeof(node));
	op_begin();
	in = open(node, O_RDONLY | O_CLOEXEC);
	if (in < 0) {
		op_fail("Backup", errno, "Cannot open %s.", node);
		return;
	}
	out = open(f[0].buf, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		op_fail("Backup", errno, "Cannot create %s.", f[0].buf);
		close(in);
		return;
	}
	buf = malloc(v->info.leb_size);
	md5_init(&m);
	prog_start(&p, "Backup", len, 1);
	while (buf && done < len) {
		long long want = len - done < v->info.leb_size ? len - done :
								 v->info.leb_size;
		ssize_t n = read(in, buf, want);
		long long i;

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			err = n < 0 ? errno : EIO;
			break;
		}
		if (write_all(out, buf, n)) {
			err = errno;
			break;
		}
		if (f[1].choice == 2) {
			for (i = n - 1; i >= 0; i--)
				if ((unsigned char)buf[i] != 0xff)
					break;
			if (i >= 0)
				last_data = done + i + 1;
		} else {
			md5_update(&m, buf, n);
		}
		done += n;
		if (prog_update(&p, done, "%s -> %s", node, f[0].buf)) {
			cancelled = 1;
			break;
		}
	}
	prog_end(&p);
	close(in);
	if (!err && !cancelled && f[1].choice == 2) {
		/* the hash covers only what is kept */
		if (ftruncate(out, last_data))
			err = errno;
		len = last_data;
	}
	if (close(out) && !err)
		err = errno;
	free(buf);
	if (err || cancelled) {
		op_fail("Backup", err, "%s after %s bytes; %s is incomplete.",
			cancelled ? "Cancelled" : "Failed", fmt_num(done, a, sizeof(a)),
			f[0].buf);
		return;
	}
	if (f[1].choice == 2) {
		int fd = open(f[0].buf, O_RDONLY | O_CLOEXEC);
		uint32_t crc;

		if (fd < 0 || hash_stream(fd, NULL, len, md5, &crc, "Backup",
					  "Hashing the file") != len)
			snprintf(md5, sizeof(md5), "?");
		if (fd >= 0)
			close(fd);
	} else {
		md5_hex(&m, md5);
	}
	ui_msg("Backup done", "Saved %s bytes of volume %d \"%s\" to %s.\n\nMD5 %s",
	       fmt_num(len, a, sizeof(a)), v->info.vol_id, v->info.name, f[0].buf, md5);
	set_status(S_OK, "Saved volume %d to %s.", v->info.vol_id, f[0].buf);
}

static int checksum_check(struct field *f, int n, char *note, size_t sz,
			  void *vctx)
{
	const struct vol *v = vctx;
	long long lebs, x;
	char err[200];

	(void)n;
	if (strcasecmp(f[1].buf, "auto")) {
		x = parse_size(f[1].buf, v->info.leb_size, v->info.data_bytes, &lebs,
			       err, sizeof(err));
		if (x < 0) {
			snprintf(note, sz, "Length: %s", err);
			return 1;
		}
		if (x > v->info.data_bytes) {
			snprintf(note, sz, "The volume has %lld bytes.", v->info.data_bytes);
			return 1;
		}
	}
	snprintf(note, sz, "auto length: the size of the file given, else %s.",
		 v->info.type == UBI_STATIC_VOLUME ? "the static volume's data" :
						     "the whole volume");
	return 0;
}

static void act_checksum(void)
{
	struct vol *v = cur_vol();
	struct field f[2] = {
		{ .type = F_TEXT, .label = "Compare with:", .browse = 1,
		  .help = "Optional: file, device or http:// URL to compare with" },
		{ .type = F_TEXT, .label = "Length:", .maxlen = 20,
		  .help = "auto, or bytes from the start (like head -c N | md5sum)" },
	};
	char node[64], md5v[33], md5f[33] = "", err[512], a[32];
	uint32_t crcv, crcf = 0;
	struct src s;
	long long len, lebs, r;
	int fd, have_file = 0;

	snprintf(f[1].buf, sizeof(f[1].buf), "auto");
	if (!ui_form("Checksum (md5sum, ubicrc32)", NULL, f, 2, checksum_check, v))
		return;
	len = v->info.data_bytes;
	if (f[0].buf[0]) {
		if (src_open(&s, f[0].buf, 0, err, sizeof(err))) {
			ui_error("Checksum", "%s", err);
			return;
		}
		have_file = 1;
		if (!strcasecmp(f[1].buf, "auto")) {
			if (s.size < 0) {
				ui_error("Checksum", "The size of %s is not known; enter the "
					 "length.", f[0].buf);
				src_close(&s);
				return;
			}
			len = s.size;
		}
	}
	if (strcasecmp(f[1].buf, "auto"))
		len = parse_size(f[1].buf, v->info.leb_size, v->info.data_bytes, &lebs,
				 err, sizeof(err));
	if (len > v->info.data_bytes) {
		ui_error("Checksum", "%s has %lld bytes, the volume only %lld.", f[0].buf,
			 len, v->info.data_bytes);
		if (have_file)
			src_close(&s);
		return;
	}
	vol_node(D.num, v->info.vol_id, node, sizeof(node));
	op_begin();
	fd = open(node, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		op_fail("Checksum", errno, "Cannot open %s.", node);
		if (have_file)
			src_close(&s);
		return;
	}
	r = hash_stream(fd, NULL, len, md5v, &crcv, "Checksum", node);
	close(fd);
	if (r != len) {
		if (r != -2)
			op_fail("Checksum", errno, "Reading %s failed.", node);
		if (have_file)
			src_close(&s);
		return;
	}
	if (have_file) {
		r = hash_stream(-1, &s, len, md5f, &crcf, "Checksum", f[0].buf);
		src_close(&s);
		if (r == -2)
			return;
		if (r != len) {
			ui_error("Checksum", "Volume: MD5 %s\n\n%s gives only %lld bytes.",
				 md5v, f[0].buf, r < 0 ? 0 : r);
			return;
		}
	}
	if (!have_file)
		ui_msg("Checksum", "First %s bytes of volume %d \"%s\":\n\nMD5      %s\n"
		       "CRC32    0x%08x (ubicrc32)", fmt_num(len, a, sizeof(a)),
		       v->info.vol_id, v->info.name, md5v, crcv);
	else if (!strcmp(md5v, md5f))
		ui_msg("Checksum: identical", "First %s bytes of volume %d \"%s\" and "
		       "%s are identical.\n\nMD5      %s\nCRC32    0x%08x",
		       fmt_num(len, a, sizeof(a)), v->info.vol_id, v->info.name,
		       f[0].buf, md5v, crcv);
	else
		ui_error("Checksum: DIFFERENT", "First %s bytes differ.\n\nVolume  MD5 "
			 "%s\nFile    MD5 %s", fmt_num(len, a, sizeof(a)), md5v, md5f);
}

struct copyctx {
	const struct vol *v;
	long long lebs, bytes;
};

static int copy_check(struct field *f, int n, char *note, size_t sz, void *vctx)
{
	struct copyctx *c = vctx;
	long long lebs_unit;
	char err[200], a[32];

	(void)n;
	if (!f[0].buf[0]) {
		snprintf(note, sz, "Enter a name.");
		return 1;
	}
	if (vol_find_name(&D, f[0].buf) >= 0) {
		snprintf(note, sz, "Volume \"%s\" already exists.", f[0].buf);
		return 1;
	}
	c->lebs = 0;
	if (parse_size(f[1].buf, D.info.leb_size, (long long)D.info.avail_lebs *
		       D.info.leb_size, &lebs_unit, err, sizeof(err)) < 0) {
		snprintf(note, sz, "%s", err);
		return 1;
	}
	c->lebs = (parse_size(f[1].buf, D.info.leb_size, (long long)D.info.avail_lebs *
			      D.info.leb_size, &lebs_unit, err, sizeof(err)) +
		   D.info.leb_size - 1) / D.info.leb_size;
	if (c->lebs * D.info.leb_size < c->bytes) {
		snprintf(note, sz, "The data (%s bytes) needs at least %lld LEBs.",
			 fmt_num(c->bytes, a, sizeof(a)),
			 (c->bytes + D.info.leb_size - 1) / D.info.leb_size);
		return 1;
	}
	if (c->lebs > D.info.avail_lebs) {
		snprintf(note, sz, "%lld LEBs needed, %d free.", c->lebs,
			 D.info.avail_lebs);
		return 1;
	}
	snprintf(note, sz, "New %s volume of %lld LEBs; %s bytes of data are "
		 "copied and verified.", f[2].choice ? "static" : "dynamic", c->lebs,
		 fmt_num(c->bytes, a, sizeof(a)));
	return 0;
}

static void act_copy(void)
{
	struct vol *v = cur_vol();
	struct copyctx c = { v, 0, content_end(v) };
	struct field f[3] = {
		{ .type = F_TEXT, .label = "New name:", .maxlen = UBI_VOL_NAME_MAX,
		  .help = "Name of the new volume" },
		{ .type = F_TEXT, .label = "Size:", .maxlen = 30,
		  .help = "Size of the new volume (default: same as the source)" },
		{ .type = F_CHOICE, .label = "Type:", .choices = vol_types,
		  .help = "Type of the new volume" },
	};
	struct ubi_mkvol_request req;
	struct src in;
	char node[64], err[512];
	int i, id;

	if (v->info.alignment != 1) {
		ui_error("Copy", "Copying volumes with alignment %d is not supported.",
			 v->info.alignment);
		return;
	}
	snprintf(f[0].buf, sizeof(f[0].buf), "%s_copy", v->info.name);
	for (i = 2; vol_find_name(&D, f[0].buf) >= 0; i++)
		snprintf(f[0].buf, sizeof(f[0].buf), "%s_copy%d", v->info.name, i);
	snprintf(f[1].buf, sizeof(f[1].buf), "%d LEB", v->info.rsvd_lebs);
	f[2].choice = v->info.type == UBI_STATIC_VOLUME;
	if (!ui_form("Copy to a new volume", NULL, f, 3, copy_check, &c))
		return;
	memset(&req, 0, sizeof(req));
	req.vol_id = UBI_VOL_NUM_AUTO;
	req.alignment = 1;
	req.bytes = c.lebs * D.info.leb_size;
	req.vol_type = f[2].choice ? UBI_STATIC_VOLUME : UBI_DYNAMIC_VOLUME;
	req.name = f[0].buf;
	dev_node(D.num, node, sizeof(node));
	op_begin();
	if (ubi_mkvol(lubi, node, &req)) {
		op_fail("Copy", errno, "Cannot create volume \"%s\".", f[0].buf);
		return;
	}
	id = req.vol_id;
	reload();
	select_vol(id);
	vol_node(D.num, v->info.vol_id, node, sizeof(node));
	for (i = 0; i < D.nvols; i++) {
		if (D.vols[i].info.vol_id == v->info.vol_id) {
			v = &D.vols[i];
			break;
		}
	}
	if (src_open(&in, node, 0, err, sizeof(err))) {
		ui_error("Copy", "Volume %d was created, but %s", id, err);
		return;
	}
	if (!do_update(cur_vol(), &in, c.bytes, 1, "Copy volume", node))
		set_status(S_OK, "Copied volume %d \"%s\" to new volume %d \"%s\".",
			   v->info.vol_id, v->info.name, id, f[0].buf);
	src_close(&in);
	reload();
}

static void act_block(void)
{
	struct vol *v = cur_vol();
	char node[64], text[300];
	int fd, r;

	vol_node(D.num, v->info.vol_id, node, sizeof(node));
	snprintf(text, sizeof(text), "%s the block device /dev/ubiblock%d_%d of "
		 "volume %d \"%s\" (ubiblock %s)?", v->block ? "Remove" : "Create",
		 D.num, v->info.vol_id, v->info.vol_id, v->info.name,
		 v->block ? "-r" : "-c");
	if (!ui_yesno("ubiblock", text, 1))
		return;
	op_begin();
	fd = open(node, O_RDONLY | O_CLOEXEC);
	r = fd < 0 ? -1 : v->block ? ubi_vol_block_remove(fd) : ubi_vol_block_create(fd);
	if (r)
		op_fail("ubiblock", errno, "Cannot %s the block device.",
			v->block ? "remove" : "create");
	else
		set_status(S_OK, "%s /dev/ubiblock%d_%d.", v->block ? "Removed" : "Created",
			   D.num, v->info.vol_id);
	if (fd >= 0)
		close(fd);
	reload();
}

/* ---- hex view ---- */

struct hexview {
	const struct vol *v;
	int fd;
	long long size, off;
	int bpl, body;
};

static void hex_draw(void *ctx)
{
	struct hexview *h = ctx;
	unsigned char buf[64 * 32];
	char title[200], line[256], a[32];
	int y, x, i, j, n, len;

	h->bpl = scr_cols >= 80 ? 16 : 8;
	snprintf(title, sizeof(title), "Hex view: volume %d \"%s\", %s bytes",
		 h->v->info.vol_id, h->v->info.name, fmt_num(h->size, a, sizeof(a)));
	ui_frame(scr_rows, scr_cols, title, S_TITLE, &y, &x);
	h->body = imax(1, scr_rows - 3);
	len = imin(h->body * h->bpl, (int)sizeof(buf));
	n = pread(h->fd, buf, len, h->off);
	if (n < 0) {
		scr_printf(y + 1, x + 2, st(S_ERR), "read at %lld: %s", h->off,
			   strerror(errno));
		n = 0;
	}
	for (i = 0; i * h->bpl < n && i < h->body; i++) {
		int k = 0;

		k += snprintf(line + k, sizeof(line) - k, "%08llx ",
			      h->off + (long long)i * h->bpl);
		for (j = 0; j < h->bpl; j++) {
			int p = i * h->bpl + j;

			if (j % 8 == 0)
				line[k++] = ' ';
			k += p < n ? snprintf(line + k, sizeof(line) - k, "%02x ", buf[p]) :
				     snprintf(line + k, sizeof(line) - k, "   ");
		}
		line[k++] = ' ';
		for (j = 0; j < h->bpl && i * h->bpl + j < n; j++) {
			unsigned char ch = buf[i * h->bpl + j];

			line[k++] = ch >= 0x20 && ch < 0x7f ? ch : '.';
		}
		line[k] = 0;
		scr_putn(y + 1 + i, x + 2, 0, line, scr_cols - 4);
	}
	snprintf(line, sizeof(line), " LEB %lld + 0x%llx   g: go to   n/p: next/prev "
		 "LEB   q: close ", h->off / h->v->info.leb_size,
		 h->off % h->v->info.leb_size);
	scr_putn(y + scr_rows - 1, x + 2, st(S_DIM), line, scr_cols - 4);
}

static int goto_check(struct field *f, int n, char *note, size_t sz, void *vctx)
{
	struct hexview *h = vctx;
	long long lebs, v;
	char err[200];

	(void)n;
	v = parse_size(f[0].buf, h->v->info.leb_size, h->size, &lebs, err,
		       sizeof(err));
	if (!strncasecmp(f[0].buf, "0x", 2))
		v = strtoll(f[0].buf, NULL, 16);
	if (v < 0) {
		snprintf(note, sz, "%s", err);
		return 1;
	}
	if (v >= h->size) {
		snprintf(note, sz, "Beyond the end (%lld bytes).", h->size);
		return 1;
	}
	snprintf(note, sz, "Offset %lld = 0x%llx", v, v);
	return 0;
}

static void act_hex(void)
{
	struct vol *v = cur_vol();
	struct hexview h = { .v = v, .size = v->info.data_bytes, .bpl = 16, .body = 1 };
	char node[64];
	int k, done = 0;

	vol_node(D.num, v->info.vol_id, node, sizeof(node));
	op_begin();
	h.fd = open(node, O_RDONLY | O_CLOEXEC);
	if (h.fd < 0) {
		op_fail("Hex view", errno, "Cannot open %s.", node);
		return;
	}
	if (h.size <= 0) {
		ui_msg("Hex view", "Volume %d \"%s\" holds no data.", v->info.vol_id,
		       v->info.name);
		close(h.fd);
		return;
	}
	ui_push(hex_draw, &h);
	while (!done) {
		long long page, last;

		ui_redraw();
		scr_flush();
		page = (long long)h.body * h.bpl;
		last = (h.size - 1) / h.bpl * h.bpl;
		k = ui_getkey(-1);
		switch (k) {
		case KEY_UP: h.off -= h.bpl; break;
		case KEY_DOWN: h.off += h.bpl; break;
		case KEY_PGUP: h.off -= page; break;
		case KEY_PGDN: case ' ': h.off += page; break;
		case KEY_HOME: h.off = 0; break;
		case KEY_END: h.off = last - page + h.bpl; break;
		case 'n': h.off = (h.off / v->info.leb_size + 1) * v->info.leb_size; break;
		case 'p':
			h.off = (h.off / v->info.leb_size - (h.off % v->info.leb_size ? 0 : 1)) *
				v->info.leb_size;
			break;
		case 'g': {
			struct field f[1] = { { .type = F_TEXT, .label = "Offset:",
						.maxlen = 30,
						.help = "Bytes, 0x hex, KiB/MiB or N LEB" } };

			if (ui_form("Go to", NULL, f, 1, goto_check, &h)) {
				long long lebs;
				char err[100];

				h.off = !strncasecmp(f[0].buf, "0x", 2) ?
						strtoll(f[0].buf, NULL, 16) :
						parse_size(f[0].buf, v->info.leb_size, h.size,
							   &lebs, err, sizeof(err));
				h.off = h.off / h.bpl * h.bpl;
			}
			break;
		}
		case 'q': case 'Q': case KEY_ESC: case KEY_HANGUP:
			done = 1;
			break;
		}
		if (h.off > last)
			h.off = last;
		if (h.off < 0)
			h.off = 0;
	}
	ui_pop();
	close(h.fd);
}

static void act_extra(void)
{
	struct vol *v = cur_vol();
	const struct list_item it[] = {
		{ "Copy to a new volume", "Create a volume with the same data", !v, 'o' },
		{ "Checksum / compare", "MD5 and CRC32, optionally against a file", !v, 'c' },
		{ "Hex view", "Browse the volume contents", !v, 'h' },
		{ "Wipe (truncate)", "Erase all data of the volume (ubiupdatevol -t)", !v, 't' },
		{ v && v->block ? "Remove ubiblock device" : "Create ubiblock device",
		  "Read-only block device for e.g. squashfs or cramfs (ubiblock)", !v, 'k' },
		{ "Swap names", "Stage renames that exchange two volume names", !v, 'p' },
		{ "Erase counter map", "Wear of every PEB (UBI_IOCECNFO)", 0, 'e' },
		{ "Read check", "Read all PEBs, scrub bit-flips (like ubihealthd)", 0, 'l' },
		{ "Refresh", "Read the device state again (also F5)", 0, 'f' },
	};
	int k = ui_list("Extra", NULL, it, 9, v ? 0 : 6);

	switch (k) {
	case 0: act_copy(); break;
	case 1: act_checksum(); break;
	case 2: act_hex(); break;
	case 3: act_wipe(); break;
	case 4: act_block(); break;
	case 5: act_swap(); break;
	case 6: wear_view(&D); break;
	case 7: health_check(&D); reload(); break;
	case 8: reload(); set_status(S_OK, "Refreshed."); break;
	}
}

/* ---- help ---- */

static void help_build(struct doc *d, int width, void *ctx)
{
	static const char *const rows[][2] = {
		{ "Up/Down, PgUp/PgDn", "select a volume or the free space" },
		{ "Left/Right, Tab", "select a menu item; Enter runs it" },
		{ "letters", "highlighted letter of a menu item runs it directly" },
		{ "F5 / Ctrl-R", "refresh" },
		{ "Ctrl-L", "redraw (and re-measure a serial terminal)" },
		{ "F1 / ?", "this help" },
	};
	static const char *const ops[][2] = {
		{ "ubinfo", "Info (volume or free space row = device)" },
		{ "ubimkvol", "New" },
		{ "ubirmvol", "Delete" },
		{ "ubirsvol", "Resize" },
		{ "ubirename", "Rename + Apply; Extra > Swap names" },
		{ "ubiupdatevol", "Write; Extra > Wipe for -t" },
		{ "ubiblock", "Extra > Create/Remove ubiblock device" },
		{ "ubicrc32, md5sum", "Extra > Checksum / compare" },
		{ "ubiattach/ubidetach", "Devices > Attach / Detach" },
		{ "ubiformat", "Devices > Format" },
		{ "ubiscan", "Devices > Scan" },
		{ "mtdinfo", "Devices > Info" },
		{ "ubihealthd", "Extra > Read check (one pass)" },
		{ "(none)", "Backup, Copy, Hex view, LEB map, erase counter map" },
	};
	size_t i;

	(void)width;
	(void)ctx;
	doc_style(d, S_TITLE);
	doc_printf(d, "ubitui %s - UBI volume manager\n\n", VERSION);
	doc_style(d, S_NORMAL);
	doc_printf(d, "Changes happen immediately, except renames: they are staged "
		   "and applied together\nin one atomic operation (Apply). Renaming a "
		   "volume to the name of an existing\nvolume that is not renamed "
		   "itself removes that volume in the same operation,\nwhich is how "
		   "images are replaced atomically. Destructive operations ask you\n"
		   "to type \"yes\".\n\n");
	doc_style(d, S_TITLE);
	doc_printf(d, "Keys\n");
	doc_style(d, S_NORMAL);
	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
		doc_printf(d, "  %-22s %s\n", rows[i][0], rows[i][1]);
	doc_printf(d, "  %-22s %s\n", "in forms", "Tab/Up/Down move, Enter OK, Esc "
		   "cancel, F2/Ctrl-B browse files");
	doc_printf(d, "\n");
	doc_style(d, S_TITLE);
	doc_printf(d, "mtd-utils equivalents\n");
	doc_style(d, S_NORMAL);
	for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
		doc_printf(d, "  %-22s %s\n", ops[i][0], ops[i][1]);
	doc_printf(d, "\nSources for Write, Checksum and Format: a file, a device such "
		   "as /dev/ubi0_3,\nhttp://host:port/path (streamed, nothing is "
		   "stored in RAM) or - for stdin,\ne.g.  wget -O - http://pc:8080/img "
		   "| ubitui\n");
	doc_printf(d, "\nTable columns: LEBs/Size reserved for the volume; Used = "
		   "mapped LEBs of a\ndynamic volume (LEBs that hold data) or the data "
		   "size of a static one.\nBar: one segment per volume (solid: used, "
		   "shaded: reserved), UBI internal\nreserves, then free space.\n");
}

/* ---- devices screen ---- */

struct mtdrow {
	struct mtd_dev_info m;
	int ubi; /* attached UBI device or -1 */
	int vols;
};

struct devscreen {
	struct mtdrow rows[64];
	int n, sel, menu;
};

static const struct menu_item dev_items[] = {
	{ "Open", 'o', ACT_NONE, "Manage the volumes of this UBI device" },
	{ "Attach", 'a', ACT_NONE, "Attach the MTD device to UBI (ubiattach)" },
	{ "Detach", 'd', ACT_NONE, "Detach it from UBI (ubidetach)" },
	{ "Format", 'f', ACT_NONE, "Erase it and write UBI headers or an image (ubiformat)" },
	{ "Scan", 's', ACT_NONE, "Read the UBI headers: erase counters (ubiscan)" },
	{ "Info", 'i', ACT_NONE, "MTD details and UBI defaults (mtdinfo -u)" },
	{ "Back", 'b', ACT_NONE, "Back to the volume list" },
	{ "Quit", 'q', ACT_NONE, "Leave ubitui" },
};

static void devs_load(struct devscreen *s)
{
	struct mtd_info mi;
	int i;

	s->n = 0;
	if (!lmtd || mtd_get_info(lmtd, &mi))
		return;
	for (i = mi.lowest_mtd_num; i <= mi.highest_mtd_num && s->n < 64; i++) {
		struct mtdrow *r = &s->rows[s->n];
		struct ubi_dev_info di;

		if (!mtd_dev_present(lmtd, i) || mtd_get_dev_info1(lmtd, i, &r->m))
			continue;
		r->ubi = -1;
		r->vols = 0;
		if (lubi && !mtd_num2ubi_dev(lubi, i, &r->ubi) &&
		    !ubi_get_dev_info1(lubi, r->ubi, &di))
			r->vols = di.vol_count;
		s->n++;
	}
	if (s->sel >= s->n)
		s->sel = imax(0, s->n - 1);
}

static void devs_draw(void *ctx)
{
	struct devscreen *s = ctx;
	char line[512], a[32], b[32];
	int i, lw = 6, per_line, mx;
	const char *t = "MTD and UBI devices";

	scr_clear();
	scr_put(0, (scr_cols - (int)strlen(t)) / 2, st(S_TITLE), t);
	snprintf(line, sizeof(line), "   %-6s %-16s %-8s %10s %9s %7s  %s", "MTD",
		 "Name", "Type", "Size", "EB size", "Min I/O", "UBI");
	scr_putn(2, 1, st(S_HEADER), line, scr_cols - 2);
	for (i = 0; i < s->n && i < scr_rows - 8; i++) {
		struct mtdrow *r = &s->rows[i];
		char ubi[64];

		if (r->ubi >= 0)
			snprintf(ubi, sizeof(ubi), "ubi%d: %d vol%s", r->ubi, r->vols,
				 r->vols == 1 ? "" : "s");
		else
			snprintf(ubi, sizeof(ubi), "-");
		snprintf(line, sizeof(line), "%s mtd%-3d %-16.16s %-8.8s %10s %9s %7d  %s",
			 i == s->sel ? ">>" : "  ", r->m.mtd_num, r->m.name, r->m.type_str,
			 fmt_size(r->m.size, a, sizeof(a)), fmt_size(r->m.eb_size, b, sizeof(b)),
			 r->m.min_io_size, ubi);
		scr_putn(3 + i, 1, i == s->sel ? A_REV : 0, line, scr_cols - 2);
	}
	if (!s->n)
		scr_put(3, 4, st(S_DIM), lmtd ? "No MTD devices." :
					 "MTD is not available in this kernel.");
	if (!lubi)
		scr_put(scr_rows - 4, 2, st(S_WARN), "UBI is not available in this "
			"kernel: attach/detach/open are not possible.");
	per_line = imin(8, (scr_cols - 2) / (lw + 6));
	for (i = 0; i < 8; i++) {
		int row = i / per_line, col = i % per_line;
		int n_in_row = imin(per_line, 8 - row * per_line);
		int lines = (8 + per_line - 1) / per_line;
		int my = scr_rows - 2 - (lines - 1) + row;
		char item[32];
		int pad = (lw - (int)strlen(dev_items[i].label)) / 2;

		mx = (scr_cols - n_in_row * (lw + 6)) / 2 + col * (lw + 6);
		snprintf(item, sizeof(item), "[ %*s%-*s ]", pad, "", lw - pad,
			 dev_items[i].label);
		scr_put(my, mx, i == s->menu ? st(S_SEL) : 0, item);
		scr_fill(my, mx + 2 + pad, 1, st(i == s->menu ? S_HOT_SEL : S_HOT),
			 dev_items[i].label[0]);
	}
	if (status[0])
		scr_putn(scr_rows - 1, 1, st(status_style), status, scr_cols - 2);
	else
		scr_put(scr_rows - 1, imax(0, (scr_cols - (int)strlen(dev_items[s->menu].help)) / 2),
			st(S_DIM), dev_items[s->menu].help);
}

static int attach_check(struct field *f, int n, char *note, size_t sz, void *ctx)
{
	char *end;
	long v;

	(void)n;
	(void)ctx;
	if (strcasecmp(f[0].buf, "auto") && f[0].buf[0]) {
		v = strtol(f[0].buf, &end, 0);
		if (*end || v < 0 || v > 31) {
			snprintf(note, sz, "UBI device number: auto or 0 to 31.");
			return 1;
		}
	}
	v = strtol(f[1].buf, &end, 0);
	if (*end || v < 0) {
		snprintf(note, sz, "VID header offset: 0 for the default.");
		return 1;
	}
	v = strtol(f[2].buf, &end, 0);
	if (*end || v < 0 || v > 768) {
		snprintf(note, sz, "Bad PEBs per 1024: 0 (kernel default) to 768.");
		return 1;
	}
	snprintf(note, sz, "UBI scans the whole MTD device while attaching.");
	return 0;
}

static int do_attach(int mtd_num, int ask)
{
	struct field f[5] = {
		{ .type = F_TEXT, .label = "UBI device:", .maxlen = 4,
		  .help = "auto, or the number of the new ubiN (ubiattach -d)" },
		{ .type = F_TEXT, .label = "VID hdr offset:", .maxlen = 8,
		  .help = "0 = default; change only if you must (ubiattach -O)" },
		{ .type = F_TEXT, .label = "Bad PEBs/1024:", .maxlen = 4,
		  .help = "Expected bad blocks per 1024 PEBs, 0 = default (ubiattach -b)" },
		{ .type = F_CHECK, .label = "No fastmap:",
		  .help = "Full scan, drop an existing fastmap (ubiattach -f)" },
		{ .type = F_CHECK, .label = "Reserve pool:",
		  .help = "Reserve PEBs for the fastmap pools (ubiattach -r)" },
	};
	struct ubi_attach_request req;
	char text[200];
	int r;

	snprintf(f[0].buf, sizeof(f[0].buf), "auto");
	snprintf(f[1].buf, sizeof(f[1].buf), "0");
	snprintf(f[2].buf, sizeof(f[2].buf), "0");
	snprintf(text, sizeof(text), "Attach mtd%d to UBI.", mtd_num);
	if (ask && !ui_form("Attach (ubiattach)", text, f, 5, attach_check, NULL))
		return -1;
	memset(&req, 0, sizeof(req));
	req.dev_num = strcasecmp(f[0].buf, "auto") ? atoi(f[0].buf) : UBI_DEV_NUM_AUTO;
	req.mtd_num = mtd_num;
	req.vid_hdr_offset = atoi(f[1].buf);
	req.max_beb_per1024 = atoi(f[2].buf);
	req.disable_fm = f[3].checked;
	req.need_resv_pool = f[4].checked;
	op_begin();
	r = ubi_attach(lubi, UBI_CTRL, &req);
	if (r < 0) {
		op_fail("Attach", errno, "Cannot attach mtd%d.", mtd_num);
		return -1;
	}
	set_status(S_OK, "Attached mtd%d as ubi%d%s.", mtd_num, req.dev_num,
		   r == 1 ? " (the kernel ignored the bad-PEB limit)" : "");
	return req.dev_num;
}

static void mtd_info_build(struct doc *d, int width, void *ctx)
{
	const struct mtdrow *r = ctx;
	struct ubigen_info g;
	char a[32], b[32];

	(void)width;
	kv(d, "MTD device", "mtd%d \"%s\" (/dev/mtd%d, %d:%d)", r->m.mtd_num, r->m.name,
	   r->m.mtd_num, r->m.major, r->m.minor);
	kv(d, "Type", "%s%s", r->m.type_str, r->m.writable ? "" : ", read-only");
	kv(d, "Size", "%s bytes (%s)", fmt_num(r->m.size, a, sizeof(a)),
	   fmt_size(r->m.size, b, sizeof(b)));
	kv(d, "Eraseblocks", "%d of %d bytes", r->m.eb_cnt, r->m.eb_size);
	kv(d, "Min. I/O unit", "%d bytes", r->m.min_io_size);
	kv(d, "Sub-page size", "%d bytes", r->m.subpage_size);
	kv(d, "OOB size", "%d bytes (%d free)", r->m.oob_size, r->m.oobavail);
	kv(d, "Erase regions", "%d", r->m.region_cnt);
	kv(d, "Bad blocks possible", "%s", r->m.bb_allowed ? "yes" : "no");
	kv(d, "Attached to", r->ubi >= 0 ? "ubi%d" : "-", r->ubi);
	ubigen_info_init(&g, r->m.eb_size, r->m.min_io_size, r->m.subpage_size, 0, 1,
			 0);
	doc_printf(d, "\n");
	doc_style(d, S_TITLE);
	doc_printf(d, "UBI defaults for this flash (mtdinfo -u)\n");
	doc_style(d, S_NORMAL);
	kv(d, "VID header offset", "%d", g.vid_hdr_offs);
	kv(d, "Data offset", "%d", g.data_offs);
	kv(d, "LEB size", "%d bytes", g.leb_size);
	kv(d, "Max volumes", "%d", g.max_volumes);
}

/* Returns the UBI device to open, -1 to go back, -2 to quit. */
static int devices_screen(int preselect_mtd)
{
	static struct devscreen s;
	int k, i, ret = -3;

	memset(&s, 0, sizeof(s));
	devs_load(&s);
	for (i = 0; i < s.n; i++)
		if (s.rows[i].m.mtd_num == preselect_mtd ||
		    (preselect_mtd < 0 && have_dev && s.rows[i].ubi == D.num))
			s.sel = i;
	ui_push(devs_draw, &s);
	while (ret == -3) {
		struct mtdrow *r;
		char text[300];
		int act = -1;

		ui_redraw();
		scr_flush();
		k = ui_getkey(-1);
		if (k != KEY_RESIZE && k != KEY_NONE)
			status[0] = 0;
		r = s.n ? &s.rows[s.sel] : NULL;
		switch (k) {
		case KEY_UP: if (s.sel > 0) s.sel--; break;
		case KEY_DOWN: if (s.sel < s.n - 1) s.sel++; break;
		case KEY_LEFT: case KEY_BTAB: s.menu = (s.menu + 7) % 8; break;
		case KEY_RIGHT: case KEY_TAB: s.menu = (s.menu + 1) % 8; break;
		case KEY_ENTER: act = s.menu; break;
		case KEY_HANGUP: ret = -2; break;
		case KEY_ESC: act = 6; break;
		case KEY_F5: case KEY_CTRL('R'): devs_load(&s); break;
		default:
			for (i = 0; i < 8; i++)
				if (k < 0x80 && k > 0 && (k | 0x20) == dev_items[i].hot)
					act = i;
		}
		if (act >= 0)
			s.menu = act;
		switch (act) {
		case 0: /* open */
			if (!r)
				break;
			if (r->ubi >= 0) {
				ret = r->ubi;
				break;
			}
			snprintf(text, sizeof(text), "mtd%d is not attached to UBI. Attach "
				 "it now?", r->m.mtd_num);
			if (lubi && ui_yesno("Open", text, 1)) {
				int dev = do_attach(r->m.mtd_num, 1);

				if (dev >= 0)
					ret = dev;
				devs_load(&s);
			}
			break;
		case 1: /* attach */
			if (!r)
				break;
			if (!lubi) {
				ui_error("Attach", "UBI is not available in this kernel.");
			} else if (r->ubi >= 0) {
				ui_error("Attach", "mtd%d is already attached as ubi%d.",
					 r->m.mtd_num, r->ubi);
			} else {
				do_attach(r->m.mtd_num, 1);
				devs_load(&s);
			}
			break;
		case 2: /* detach */
			if (!r)
				break;
			if (r->ubi < 0) {
				ui_error("Detach", "mtd%d is not attached.", r->m.mtd_num);
				break;
			}
			snprintf(text, sizeof(text), "Detach mtd%d from ubi%d? Its %d "
				 "volume%s become unavailable until it is attached again; "
				 "mounted volumes make this fail.", r->m.mtd_num, r->ubi,
				 r->vols, r->vols == 1 ? "" : "s");
			if (!ui_yesno("Detach (ubidetach)", text, 0))
				break;
			op_begin();
			if (ubi_detach_mtd(lubi, UBI_CTRL, r->m.mtd_num)) {
				op_fail("Detach", errno, "Cannot detach mtd%d.", r->m.mtd_num);
			} else {
				set_status(S_OK, "Detached mtd%d (was ubi%d).", r->m.mtd_num,
					   r->ubi);
				if (have_dev && D.num == r->ubi) {
					have_dev = 0;
					nren = 0;
				}
			}
			devs_load(&s);
			break;
		case 3: /* format */
			if (!r)
				break;
			i = format_mtd(r->m.mtd_num);
			if (i >= 0)
				set_status(S_OK, "Formatted mtd%d.", r->m.mtd_num);
			if (i == 1 && lubi) {
				int dev = do_attach(r->m.mtd_num, 0);

				if (dev >= 0)
					ret = dev;
			}
			devs_load(&s);
			break;
		case 4: /* scan */
			if (r)
				scan_view(r->m.mtd_num);
			break;
		case 5: /* info */
			if (r) {
				snprintf(text, sizeof(text), "mtd%d (mtdinfo)", r->m.mtd_num);
				ui_view(text, mtd_info_build, r);
			}
			break;
		case 6: /* back */
			if (have_dev)
				ret = -1;
			else
				set_status(S_WARN, "No UBI device is open: Open one or Quit.");
			break;
		case 7:
			ret = -2;
			break;
		}
	}
	ui_pop();
	return ret;
}

/* ---- main loop ---- */

static int open_dev(int num)
{
	struct dev nd;

	op_begin();
	if (dev_load(&nd, num)) {
		op_fail("Open", errno, "Cannot read ubi%d.", num);
		return -1;
	}
	if (!have_dev || D.num != num)
		nren = 0;
	memcpy(&D, &nd, sizeof(D)); /* struct mtd_dev_info has const members */
	have_dev = 1;
	sel = D.nvols ? 0 : D.nvols;
	top = 0;
	menu_sel = 0;
	return 0;
}

static void go_devices(int preselect_mtd)
{
	for (;;) {
		int r = devices_screen(preselect_mtd);

		if (r == -2) {
			quit = 1;
			return;
		}
		if (r == -1 && have_dev)
			return;
		if (r >= 0 && !open_dev(r))
			return;
		preselect_mtd = -1;
	}
}

static void act_quit(void)
{
	char text[200];

	if (nren) {
		snprintf(text, sizeof(text), "%d staged rename%s not applied. Quit "
			 "anyway?", nren, nren > 1 ? "s are" : " is");
		if (!ui_yesno("Quit", text, 0))
			return;
	}
	quit = 1;
}

static void run(enum act a)
{
	struct vol *v = cur_vol();

	switch (a) {
	case ACT_INFO: act_info(); break;
	case ACT_NEW: act_new(); break;
	case ACT_DELETE: case ACT_RESIZE: case ACT_RENAME: case ACT_WRITE:
	case ACT_BACKUP:
		if (!v) {
			set_status(S_WARN, "Select a volume first.");
			break;
		}
		if (a == ACT_DELETE) act_delete();
		else if (a == ACT_RESIZE) act_resize();
		else if (a == ACT_RENAME) act_rename();
		else if (a == ACT_WRITE) act_write();
		else act_backup();
		break;
	case ACT_EXTRA: act_extra(); break;
	case ACT_DEVICES: go_devices(-1); break;
	case ACT_APPLY:
		if (nren)
			act_apply();
		break;
	case ACT_UNDO:
		if (nren) {
			set_status(S_OK, "Dropped %d staged rename%s.", nren,
				   nren > 1 ? "s" : "");
			nren = 0;
		}
		break;
	case ACT_HELP: ui_view("Help", help_build, NULL); break;
	case ACT_QUIT: act_quit(); break;
	case ACT_NONE: break;
	}
}

static void main_loop(void)
{
	while (!quit) {
		const struct menu_item *menu[16];
		int k, n, i;

		if (!have_dev) {
			go_devices(-1);
			continue;
		}
		ui_redraw();
		scr_flush();
		k = ui_getkey(-1);
		if (k != KEY_RESIZE && k != KEY_NONE)
			status[0] = 0;
		n = build_menu(menu);
		switch (k) {
		case KEY_UP: if (sel > 0) sel--; break;
		case KEY_DOWN: if (sel < D.nvols) sel++; break;
		case KEY_PGUP: sel = imax(0, sel - 10); break;
		case KEY_PGDN: sel = imin(D.nvols, sel + 10); break;
		case KEY_HOME: sel = 0; break;
		case KEY_END: sel = D.nvols; break;
		case KEY_LEFT: case KEY_BTAB: menu_sel = (menu_sel + n - 1) % n; break;
		case KEY_RIGHT: case KEY_TAB: menu_sel = (menu_sel + 1) % n; break;
		case KEY_ENTER: run(menu[imin(menu_sel, n - 1)]->act); break;
		case KEY_F1: case '?': run(ACT_HELP); break;
		case KEY_F5: case KEY_CTRL('R'): reload(); set_status(S_OK, "Refreshed."); break;
		case KEY_HANGUP: quit = 1; break;
		case KEY_ESC: break;
		default:
			if (k <= 0 || k >= 0x80)
				break;
			for (i = 0; i < (int)(sizeof(all_items) / sizeof(all_items[0])); i++) {
				if ((k | 0x20) == all_items[i].hot) {
					run(all_items[i].act);
					break;
				}
			}
		}
		/* keep the menu cursor on the same action when the row type changes */
		n = build_menu(menu);
		if (menu_sel >= n)
			menu_sel = n - 1;
	}
}

static void usage(FILE *f)
{
	fprintf(f, "Usage: ubitui [-a] [-m] [UBI device | MTD device]\n"
		"  UBI device: /dev/ubi0, ubi0 or 0; MTD device: /dev/mtd1 or mtd1\n"
		"  -a  ASCII lines (for terminals without DEC line drawing)\n"
		"  -m  monochrome (also with NO_COLOR set)\n"
		"Terminal UI for UBI volumes (ubitui %s). Input from the terminal; \"-\"\n"
		"as a write source reads data piped into stdin.\n", VERSION);
}

int main(int argc, char **argv)
{
	int c, ascii = 0, mono = getenv("NO_COLOR") != NULL, want_ubi = -1;
	int want_mtd = -1;
	struct ubi_info ui;

	while ((c = getopt(argc, argv, "amhV")) != -1) {
		switch (c) {
		case 'a': ascii = 1; break;
		case 'm': mono = 1; break;
		case 'V': printf("ubitui %s\n", VERSION); return 0;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (optind < argc) {
		const char *a = argv[optind];

		if (!strncmp(a, "/dev/", 5))
			a += 5;
		if (!strncmp(a, "mtd", 3))
			want_mtd = atoi(a + 3);
		else
			want_ubi = atoi(!strncmp(a, "ubi", 3) ? a + 3 : a);
	}
	backend_open();
	if (!lubi && !lmtd) {
		fprintf(stderr, "ubitui: neither UBI nor MTD is available\n");
		return 1;
	}
	errcap_init();
	if (term_open(ascii, mono)) {
		fprintf(stderr, "ubitui: needs a terminal (/dev/tty)\n");
		return 1;
	}
	ui_push(main_draw, NULL);
	if (want_ubi < 0 && want_mtd < 0 && lubi && !ubi_get_info(lubi, &ui) &&
	    ui.dev_count > 0)
		want_ubi = ui.lowest_dev_num;
	if (want_ubi >= 0)
		open_dev(want_ubi);
	if (!have_dev)
		go_devices(want_mtd);
	main_loop();
	term_close();
	return 0;
}
