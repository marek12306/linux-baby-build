#!/bin/sh
# Builds out/zImage-<release>-drm-appended: the linux-baby kernel with the
# Buildroot initramfs embedded and the DRM (imx-lcdc) device tree appended,
# i.e. the image RedBoot loads. The kernel and Buildroot are the commits the
# submodules linux/ and buildroot/ point to; both are built out of tree.
#
# The kernel embeds the initramfs and the initramfs carries kernel modules,
# so the kernel is built twice from the same tree:
#   1. Buildroot toolchain (external Bootlin musl toolchain)
#   2. kernel modules (and the kernel release they are built for)
#   3. static wpa_supplicant and ubitui
#   4. Buildroot initramfs (post-build.sh adds 2 and 3)
#   5. zImage with the initramfs, DTB appended
#
# Environment:
#   WORK       work directory (default ./work); downloads in $WORK/dl
#   JOBS       parallel jobs (default: nproc)
set -eu

TOP=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$TOP/work}
OUT=$TOP/out
JOBS=${JOBS:-$(nproc)}
DL=${BR2_DL_DIR:-$WORK/dl}
BUILDROOT=$TOP/buildroot
BR_OUT=$WORK/buildroot-output
LINUX=$TOP/linux
KOUT=$WORK/linux-build
CROSS=$BR_OUT/host/bin/arm-linux-
DTB=nxp/imx/imx25-squeezebox-radio.dtb # DRM; -fbdev.dtb is the imxfb variant

export BR2_DL_DIR=$DL

step() {
	printf '\n=== %s\n' "$*"
}

step "submodules"
cd "$TOP"
git submodule sync -q
git submodule update --init --depth 1 buildroot
# scripts/setlocalversion counts the commits since the release tag (v7.3
# for 7.3.0) for the kernel release 7.3.0-NNNNN-g<hash>, so the history
# down to that tag is needed, plus the tag itself. The submodule is cloned
# with depth 1 (shallow = true); fetching its commit again deepens it.
git submodule update --init linux
tag=v$(sed -n 's/^VERSION = //p;s/^PATCHLEVEL = //p' "$LINUX/Makefile" |
	paste -sd.)
git -C "$LINUX" fetch -q --depth 64 origin "$(git -C "$LINUX" rev-parse HEAD)" \
	"refs/tags/$tag:refs/tags/$tag"
if ! git -C "$LINUX" merge-base --is-ancestor "$tag" HEAD; then
	echo "linux: $tag is not within 64 commits of HEAD; raise --depth" >&2
	exit 1
fi

step "Buildroot toolchain"
make -s -C "$BUILDROOT" O="$BR_OUT" BR2_EXTERNAL="$TOP/br2-external" \
	squeezebox_radio_defconfig
make -C "$BR_OUT" toolchain

step "kernel modules"
kmake() {
	make -s -C "$LINUX" O="$KOUT" ARCH=arm CROSS_COMPILE="$CROSS" -j"$JOBS" "$@"
}
kmake squeezebox_radio_defconfig
kmake vmlinux modules # modpost resolves module symbols against vmlinux
release=$(cat "$KOUT/include/config/kernel.release")
echo "kernel release $release"

step "wpa_supplicant and ubitui"
export BUILDROOT CROSS DL
OUTDIR=$WORK/wpa_supplicant sh "$TOP/scripts/build-wpa-supplicant.sh"
OUTDIR=$WORK/ubitui sh "$TOP/tools/ubitui/build.sh"

step "initramfs"
export SQUEEZEBOX_LINUX="$KOUT"
export SQUEEZEBOX_WPA="$WORK/wpa_supplicant/wpa_supplicant"
export SQUEEZEBOX_UBITUI="$WORK/ubitui/ubitui"
make -C "$BR_OUT"

step "zImage with the initramfs"
"$LINUX/scripts/config" --file "$KOUT/.config" \
	--set-str INITRAMFS_SOURCE "$BR_OUT/images/rootfs.cpio"
kmake olddefconfig
kmake zImage dtbs
if [ "$(cat "$KOUT/include/config/kernel.release")" != "$release" ]; then
	echo "kernel release changed during the build; modules would not load" >&2
	exit 1
fi

image=zImage-$release-drm-appended
mkdir -p "$OUT"
cat "$KOUT/arch/arm/boot/zImage" "$KOUT/arch/arm/boot/dts/$DTB" > "$OUT/$image"
cp "$KOUT/.config" "$OUT/config-$release"
(cd "$OUT" && md5sum "$image" > "$image.md5")
ls -l "$OUT"
cat "$OUT/$image.md5"
