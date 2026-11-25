# Third-party content

This repository is licensed under GPL-2.0-or-later ([COPYING](COPYING)),
Copyright (C) 2026 deepivin, except as listed here.

## Files in this repository

| Path | Origin | License |
|---|---|---|
| `tools/ubitui/flash.c` | scan and format code follows mtd-utils 2.2.1 `ubi-utils/ubiformat.c` and `lib/libscan.c` (Artem Bityutskiy, Nokia) | GPL-2.0-or-later |
| `tools/ubitui/` (binary) | statically links libubi, libmtd, libubigen and the CRC32 code of mtd-utils 2.2.1, downloaded at build time | GPL-2.0-or-later |
| `br2-external/board/squeezebox-radio/rootfs-overlay/squeezeos/Networking.lua` | SqueezePlay `jive/net/Networking.lua` (SqueezeOS 9.0.1 r17084), Copyright 2010 Logitech, with the ath6kl detection added | BSD-3-Clause, text below |
| `br2-external/board/squeezebox-radio/rootfs-overlay/squeezeos/rcS` | adapted from the SqueezeOS `squeezeos-base-files` `rcS` | GPL (the SqueezeOS recipe gives no version) |
| `br2-external/board/squeezebox-radio/rootfs-overlay/etc/asound.conf` | SqueezeOS `squeezeos-base-files` ALSA configuration | GPL (as above) |
| `br2-external/board/squeezebox-radio/rootfs-overlay/usr/lib/alsa-lib/libasound_module_pcm_babydsp.so` | built from [babydsp](https://github.com/marek12306/babydsp) | LGPL-2.1-or-later; see babydsp's NOTICE |

## Used by the build, not stored here

| Component | Where | License |
|---|---|---|
| linux-baby kernel | submodule `linux/` | GPL-2.0 |
| Buildroot | submodule `buildroot/` | GPL-2.0-or-later |
| Buildroot packages (BusyBox, musl, alsa-lib, alsa-utils, mpg123, ethtool, ...) | downloaded to `work/dl` | per package; `make -C work/buildroot-output legal-info` lists them |
| wpa_supplicant 2.11 | downloaded to `work/dl` | BSD-3-Clause |
| libnl 3.11.0 | downloaded to `work/dl` | LGPL-2.1 |
| mtd-utils 2.2.1 | downloaded to `work/dl` | GPL-2.0-or-later |

The kernel image contains GPL-licensed software (the kernel, BusyBox,
ubitui). Whoever distributes an image must also make the corresponding
sources available: the submodule commits and the files in `work/dl`.

## SqueezePlay license (Networking.lua)

```
Copyright 2010, Logitech, inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Logitech nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL LOGITECH, INC BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
