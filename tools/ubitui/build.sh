#!/bin/sh
# Static ubitui for the radio (musl, Buildroot toolchain), linked against
# libubi/libmtd/libubigen from mtd-utils 2.2.1 (Buildroot's version and
# hash). The SqueezeOS root is glibc without these libraries, hence static.
#
# Environment (set by build.sh):
#   BUILDROOT  Buildroot source tree (package hashes)
#   CROSS      toolchain prefix, e.g. .../host/bin/arm-linux-
#   DL         download directory
#   OUTDIR     output directory; the binary is $OUTDIR/ubitui
set -eu

: "${BUILDROOT:?}" "${CROSS:?}" "${DL:?}" "${OUTDIR:?}"
WORK=$OUTDIR/work
MTD=mtd-utils-2.2.1

if [ ! -f "$DL/$MTD.tar.bz2" ]; then
	mkdir -p "$DL"
	curl -sSfL -o "$DL/$MTD.tar.bz2" "https://infraroot.at/pub/mtd/$MTD.tar.bz2"
fi
sum=$(sed -n "s/^sha256  *\([0-9a-f]*\)  *$MTD.tar.bz2\$/\1/p" \
	"$BUILDROOT/package/mtd/mtd.hash")
echo "$sum  $DL/$MTD.tar.bz2" | sha256sum -c -

rm -rf "$WORK"
mkdir -p "$WORK/obj"
tar -xjf "$DL/$MTD.tar.bz2" -C "$WORK"

make -C "$(dirname "$0")" -j"$(nproc)" CC="${CROSS}gcc" \
	MTD_SRC="$WORK/$MTD" O="$WORK/obj" LDFLAGS=-static
"${CROSS}strip" -o "$OUTDIR/ubitui" "$WORK/obj/ubitui"
ls -l "$OUTDIR/ubitui"
