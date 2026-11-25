#!/bin/sh
# Static wpa_supplicant with the nl80211 driver for the linux-baby WLAN.
#
# The stock SqueezeOS wpa_supplicant 2.9 has only the wext driver. ath6kl is
# a fullMAC (connect/disconnect) cfg80211 driver; cfg80211's wireless
# extensions emulation turns each SIOCSIWFREQ/SIOCSIWAP/SIOCSIWESSID into its
# own connect request and rejects the next one with -EALREADY, so joining a
# network from SqueezePlay fails. nl80211 has one explicit connect command.
#
# The SqueezeOS root is glibc without libnl, and the musl initramfs is freed
# by switch_root, so the binary is linked statically (musl, libnl, internal
# crypto). Sources: Buildroot's wpa_supplicant 2.11 and libnl 3.11.0 with the
# Buildroot patches and hashes.
#
# Environment (set by build.sh):
#   BUILDROOT  Buildroot source tree (package hashes and patches)
#   CROSS      toolchain prefix, e.g. .../host/bin/arm-linux-
#   DL         download directory
#   OUTDIR     output directory; the binary is $OUTDIR/wpa_supplicant
set -eu

: "${BUILDROOT:?}" "${CROSS:?}" "${DL:?}" "${OUTDIR:?}"
PKG=$BUILDROOT/package
WORK=$OUTDIR/work

WPA=wpa_supplicant-2.11
LIBNL=libnl-3.11.0

fetch() {
	file=$1
	url=$2
	pkg=$3
	if [ ! -f "$DL/$file" ]; then
		mkdir -p "$DL"
		curl -sSfL -o "$DL/$file" "$url"
	fi
	sum=$(sed -n "s/^sha256  *\([0-9a-f]*\)  *$file\$/\1/p" "$PKG/$pkg/$pkg.hash")
	echo "$sum  $DL/$file" | sha256sum -c -
}

fetch $WPA.tar.gz https://w1.fi/releases/$WPA.tar.gz wpa_supplicant
fetch $LIBNL.tar.gz \
	https://github.com/thom311/libnl/releases/download/libnl3_11_0/$LIBNL.tar.gz \
	libnl

rm -rf "$WORK"
mkdir -p "$WORK"
tar -xzf "$DL/$LIBNL.tar.gz" -C "$WORK"
tar -xzf "$DL/$WPA.tar.gz" -C "$WORK"
for p in "$PKG"/libnl/*.patch; do
	patch -d "$WORK/$LIBNL" -p1 -s < "$p"
done
for p in "$PKG"/wpa_supplicant/*.patch; do
	patch -d "$WORK/$WPA" -p1 -s < "$p"
done

(
	cd "$WORK/$LIBNL"
	./configure --host=arm-buildroot-linux-musleabi CC="${CROSS}gcc" \
		--prefix="$WORK/libnl" --enable-static --disable-shared \
		--disable-cli --disable-debug >/dev/null
	make -j"$(nproc)" >/dev/null
	make install >/dev/null
)

cat > "$WORK/$WPA/wpa_supplicant/.config" <<'EOF'
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL32=y
CONFIG_CTRL_IFACE=y
CONFIG_BACKEND=file
CONFIG_TLS=internal
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_IEEE8021X_EAPOL=y
CONFIG_WPS=y
CONFIG_DEBUG_FILE=y
CONFIG_DEBUG_SYSLOG=y
CONFIG_NO_CONFIG_BLOBS=y
EOF

make -C "$WORK/$WPA/wpa_supplicant" -j"$(nproc)" CC="${CROSS}gcc" \
	LIBNL_INC="$WORK/libnl/include/libnl3" \
	EXTRA_CFLAGS="-Os" \
	LDFLAGS="-static -L$WORK/libnl/lib" \
	LIBS_EXTRA="-lpthread -lm" \
	wpa_supplicant >/dev/null

"${CROSS}strip" -o "$OUTDIR/wpa_supplicant" "$WORK/$WPA/wpa_supplicant/wpa_supplicant"
ls -l "$OUTDIR/wpa_supplicant"
