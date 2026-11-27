#!/bin/sh
# babydsp crossover ALSA plugin for the SqueezeOS root (glibc 2.11.1,
# alsa-lib 1.0.18), built by the babydsp Makefile with Bootlin's ARMv5 EABI
# glibc toolchain against alsa-lib 1.0.18. The toolchain's glibc is newer
# than the radio's, but ARM EABI glibc versions every function the plugin
# calls at GLIBC_2.4; the result is checked for that.
#
# Environment (set by build.sh):
#   BABYDSP  babydsp source tree (submodule)
#   DL       download directory
#   OUTDIR   output directory; the plugin is
#            $OUTDIR/libasound_module_pcm_babydsp.so
set -eu

: "${BABYDSP:?}" "${DL:?}" "${OUTDIR:?}"
WORK=$OUTDIR/work
PLUGIN=libasound_module_pcm_babydsp.so

TC=armv5-eabi--glibc--stable-2024.05-1
TC_SHA256=13f2bea0b06fe560fe60d199cd9d3fb1f20e2335bcc79b90c5130c3916a3ed92
ALSA=alsa-lib-1.0.18.tar.bz2 # checked by babydsp's alsa-lib.sh

fetch() {
	if [ ! -f "$DL/$1" ]; then
		mkdir -p "$DL"
		curl -sSfL -o "$DL/$1.part" "$2"
		mv "$DL/$1.part" "$DL/$1"
	fi
}

fetch "$TC.tar.xz" \
	"https://toolchains.bootlin.com/downloads/releases/toolchains/armv5-eabi/tarballs/$TC.tar.xz"
echo "$TC_SHA256  $DL/$TC.tar.xz" | sha256sum -c -
fetch "$ALSA" "https://www.alsa-project.org/files/pub/lib/$ALSA"

rm -rf "$WORK"
mkdir -p "$WORK/babydsp/build-alsa-lib"
tar -xJf "$DL/$TC.tar.xz" -C "$WORK"
# Out of tree: the Makefile writes sysroot/ and build-alsa-lib/ next to it.
cp "$BABYDSP/Makefile" "$BABYDSP/alsa-lib.sh" "$BABYDSP/babydsp.c" \
	"$WORK/babydsp/"
cp "$DL/$ALSA" "$WORK/babydsp/build-alsa-lib/"

# alsa-lib.sh derives configure's --host from the prefix: no directory in it.
export PATH="$WORK/$TC/bin:$PATH"
make -C "$WORK/babydsp" CROSS_COMPILE=arm-linux- "$PLUGIN"

versions=$(arm-linux-readelf -V "$WORK/babydsp/$PLUGIN" |
	grep -o 'GLIBC_[0-9.]*' | sort -u | paste -sd' ')
if [ "$versions" != GLIBC_2.4 ]; then
	echo "$PLUGIN needs $versions; the radio's glibc needs GLIBC_2.4 only" >&2
	exit 1
fi
cp "$WORK/babydsp/$PLUGIN" "$OUTDIR/"
ls -l "$OUTDIR/$PLUGIN"
