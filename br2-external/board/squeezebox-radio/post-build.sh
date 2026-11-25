#!/bin/sh
# Buildroot post-build script: add the binaries linux-baby needs beyond the
# stock system to the initramfs, which hands them to the SqueezeOS root from RAM:
# - cfg80211 and ath6kl modules, gzipped (the module directory stays in RAM;
#   the wlan script unpacks each module to /tmp only while loading it);
# - the static nl80211 wpa_supplicant from scripts/build-wpa-supplicant.sh;
# - the static ubitui from tools/ubitui/build.sh, shown as /sbin/ubitui.
#
# build.sh runs this through Buildroot and exports:
#   SQUEEZEBOX_LINUX    kernel build directory with the modules already built
#   SQUEEZEBOX_WPA      wpa_supplicant binary
#   SQUEEZEBOX_UBITUI   ubitui binary
set -eu

TARGET_DIR=$1
: "${SQUEEZEBOX_LINUX:?run build.sh}" "${SQUEEZEBOX_WPA:?}" "${SQUEEZEBOX_UBITUI:?}"

release=$(cat "$SQUEEZEBOX_LINUX/include/config/kernel.release")
dest=$TARGET_DIR/lib/modules/$release

rm -rf "$TARGET_DIR/lib/modules"
mkdir -p "$dest"
for ko in net/wireless/cfg80211.ko \
	drivers/net/wireless/ath/ath6kl/ath6kl_core.ko \
	drivers/net/wireless/ath/ath6kl/ath6kl_sdio.ko; do
	"$HOST_DIR/bin/arm-linux-strip" --strip-unneeded -o "$dest/${ko##*/}" \
		"$SQUEEZEBOX_LINUX/$ko"
	gzip -9 -n "$dest/${ko##*/}"
done

install -m 0755 "$SQUEEZEBOX_WPA" "$TARGET_DIR/squeezeos/wpa_supplicant"
install -m 0755 "$SQUEEZEBOX_UBITUI" "$TARGET_DIR/squeezeos/ubitui"
