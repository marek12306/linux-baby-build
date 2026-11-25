# linux-baby-build

Builds the Linux kernel image for the Logitech Squeezebox Radio ("baby"):
the [linux-baby](https://github.com/marek12306/linux-baby) kernel, a
downstream Linux 7.3 with Squeezebox Radio support, with a Buildroot initramfs
embedded and the DRM device tree appended, as one file that RedBoot loads. The
initramfs mounts the stock SqueezeOS root (CramFS plus the `ubifs` settings
volume) and adds what the new kernel needs on top of it, so SqueezePlay runs
unchanged.

Installing the image on a radio: [docs/INSTALL.md](docs/INSTALL.md).

## Building

On Debian or Ubuntu:

```sh
sudo apt-get install build-essential bc bison flex libssl-dev libelf-dev \
    cpio rsync unzip file wget curl git python3
git clone https://github.com/marek12306/linux-baby-build.git
cd linux-baby-build
./build.sh
```

The result is in `out/`:

| File | |
|---|---|
| `zImage-<release>-drm-appended` | the image, e.g. `zImage-7.3.0-00016-g69012f415ebe-drm-appended` |
| `zImage-<release>-drm-appended.md5` | its checksum |
| `config-<release>` | the kernel configuration used |

The kernel and Buildroot are git submodules: `linux/`
([linux-baby](https://github.com/marek12306/linux-baby), branch `baby`) and
`buildroot/` (upstream Buildroot, 2025.02.3). `build.sh` checks out the
commits they point to shallowly (the kernel down to its release tag, which the
kernel release is counted from), builds both out of tree in `work/` and
downloads all other sources to `work/dl`, checked against Buildroot's hashes.
A first build takes about 9 minutes on 8 cores and 3.5 GB in `work/`.
Variables: `WORK` (work directory), `JOBS`. Building as root (e.g. in a
container) needs `FORCE_UNSAFE_CONFIGURE=1` for Buildroot.

To build another kernel commit, check it out in `linux/` (or move the
submodule: `git -C linux fetch origin baby && git -C linux checkout FETCH_HEAD`,
then commit `linux`).

The kernel embeds the initramfs and the initramfs carries kernel modules, so
the kernel is built twice from the same tree: modules first, then, after
Buildroot, the `zImage` with the initramfs. The kernel configuration is
`squeezebox_radio_defconfig` from linux-baby with `CONFIG_INITRAMFS_SOURCE`
set to the Buildroot `rootfs.cpio`.

GitHub Actions runs `build.sh` on every push
([`.github/workflows/build.yml`](.github/workflows/build.yml)) and publishes
`out/` as the `zImage` artifact.

## Contents

| Path | |
|---|---|
| `build.sh` | the build |
| `linux/`, `buildroot/` | submodules: kernel and Buildroot |
| `br2-external/` | Buildroot external tree: `squeezebox_radio_defconfig`, `post-build.sh`, rootfs overlay |
| `scripts/build-wpa-supplicant.sh` | static wpa_supplicant 2.11 with nl80211 |
| `tools/ubitui/` | cfdisk-like UBI volume manager, built static |
| `docs/INSTALL.md` | testing and installing an image |

What the initramfs adds to the SqueezeOS root (from RAM; nothing is written to
the flash):

| File in the initramfs | In the SqueezeOS root | |
|---|---|---|
| `init` | — | mounts CramFS (through ubiblock) and per-directory overlays with the `ubifs` volume, then switches to SqueezeOS |
| `squeezeos/rcS` | `/etc/init.d/rcS` | SqueezeOS 9.0.1 r17084 boot script adapted to linux-baby |
| `squeezeos/wlan` | `/etc/init.d/wlan` | loads ath6kl and starts `wpa_supplicant` |
| `squeezeos/wpa_supplicant` | `/usr/sbin/wpa_supplicant` | static, nl80211 |
| `squeezeos/Networking.lua` | `/usr/share/jive/jive/net/Networking.lua` | SqueezePlay file with ath6kl detection (only over the known r17084 file) |
| `lib/modules/` | `/lib/modules` | cfg80211, ath6kl (gzipped) |
| `usr/lib/alsa-lib/libasound_module_pcm_babydsp.so` | same path | crossover plugin, prebuilt from [babydsp](https://github.com/marek12306/babydsp) (md5 `2e4d1384ca7788ecef77352df3ca48bf`) |
| `squeezeos/ubitui` | `/sbin/ubitui` | in a RAM layer under the `/sbin` overlay |

The babydsp plugin must match SqueezeOS's glibc and alsa-lib 1.0.18, which
needs an old glibc toolchain; it is therefore committed prebuilt instead of
being built here.

## License

GPL-2.0-or-later ([COPYING](COPYING)), except the files listed in
[THIRD-PARTY.md](THIRD-PARTY.md) (SqueezePlay's `Networking.lua` is
BSD-3-Clause, the babydsp plugin LGPL-2.1-or-later).
