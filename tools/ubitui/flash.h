/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UBITUI_FLASH_H
#define UBITUI_FLASH_H

#include "ubi.h"

void scan_view(int mtd_num);
void wear_view(const struct dev *d);
void health_check(const struct dev *d);
/* Returns 1 if formatted and the user wants it attached, 0 if formatted,
 * -1 if cancelled or failed. */
int format_mtd(int mtd_num);

#endif
