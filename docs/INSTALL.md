# Installing the linux-baby kernel on a Squeezebox Radio

This guide replaces the stock 2.6.26 kernel of a Logitech Squeezebox Radio with [linux-baby](https://github.com/marek12306/linux-baby), a downstream Linux 7.3 kernel with Squeezebox Radio support, built by this repository. SqueezePlay and the rest of the stock SqueezeOS userspace keep running unchanged on top of it.

What changes on the flash:

- One new UBI volume holding the new kernel. A rename then makes RedBoot boot it.
- The stock kernel stays intact under the name `kernel_stock`. Nothing else is written: no RedBoot, no RedBoot configuration, no CramFS root filesystem, no factory reset.
- Settings stay where stock firmware keeps them, in the `ubifs` volume, so both kernels share them.

Nothing else needs installing. Every file the new kernel needs beyond the stock system is inside the kernel image (its built-in initramfs) and is placed into the SqueezeOS root from RAM at boot, so stock firmware never sees it:

- the boot script `rcS`;
- the WLAN start script;
- a static `wpa_supplicant` 2.11;
- the WLAN kernel modules;
- a small `Networking.lua` patch;
- the `babydsp` ALSA plugin;
- `ubitui` (see below).

## Requirements

| What | Why / how to check |
|---|---|
| SqueezeOS **9.0.1 r17084** on the radio | `cat /etc/squeezeos.version`. The only version tested. Other versions should boot, but their root filesystem may not match the `rcS` copied from r17084, and the `Networking.lua` patch is skipped (SqueezePlay then does not recognise the WLAN chip). |
| Board revision below 9, for WLAN | RedBoot banner `System type 2070 revision N`, or `Revision` in `/proc/cpuinfo`. Revision 9 and later have an AR6003, which is not supported. Everything else still works on those boards. |
| Ethernet cable and DHCP on the LAN | RedBoot only uses Ethernet. It asks for an address by BOOTP while PAUSE is held. |
| A Linux PC on the same LAN | Serves the image over HTTP (`python3`). |
| 3.3 V USB-to-serial adapter | **3.3 V only.** Use the battery connector inside the battery compartment: pin 6 = radio TX, pin 8 = radio RX, pin 9 = GND ([pinout](https://bufferunderrun.co.uk/squeezebox/2020/06/29/Squeezebox-Radio-serial-console.html)). Settings 115200 8N1, for example `screen /dev/ttyUSB0 115200`. |

The serial console is what makes this safe: it is the only way back if a kernel does not boot. A variant without it is described at the end.

## 1. Get the image

The image is `zImage-<release>-drm-appended`: the kernel with its initramfs and the device tree appended, as RedBoot loads it. It comes with an `.md5` file.

- **Prebuilt:** the `zImage` artifact of a successful "Build zImage" run in this repository's GitHub Actions.
- **Built locally:** `./build.sh` (see `README.md`) writes it to `out/`.

Below, `<image>` is its file name, `<size>` its size in bytes (`stat -c %s <image>`) and `<md5>` the checksum from the `.md5` file.

## 2. Serve the image

```sh
python3 -m http.server 8080
```

## 3. Test from RAM (writes nothing)

1. Connect the serial adapter, Ethernet and power. Hold **PAUSE** while switching the radio on, until the `RedBoot>` prompt appears. RedBoot prints its address (`IP: …`) obtained by BOOTP. If it shows none, it cannot load over HTTP.
2. Load and start the kernel with exactly the command line the stock boot script uses:

   ```
   load -r -b 0x00100000 -m HTTP -h <pc-ip> -p 8080 <image>
   exec -c "console=ttymxc1,115200 noinitrd init=/linuxrc ubi.mtd=1 root=/dev/mtdblock:cramfs"
   ```

3. Expect the Lyrion boot logo, then `Starting SqueezePlay` on the console and the normal SqueezePlay UI. Check playback, the volume knob, the wheel and the buttons.
   - The boot log on the console should contain `root: wlan started` (board revision below 9 only).
   - Press Enter on the console for a root shell. `uname -r` should print `7.3.0-…`.

A power cycle without PAUSE boots stock again: nothing has been written yet.

## 4. Write the kernel into a new UBI volume

In the root shell of the RAM-booted linux-baby system:

```sh
for v in /sys/class/ubi/ubi0_*; do echo "$v $(cat $v/name) $(cat $v/reserved_ebs)"; done
cat /sys/class/ubi/ubi0/avail_eraseblocks      # needs at least 96
ubimkvol /dev/ubi0 -N kernel_new -s 12386304   # 96 LEBs of 129024 bytes
```

`ubimkvol` prints the new volume ID (`Volume ID 5, …` on a stock radio). Use it in place of `<id>`:

```sh
wget -O - http://<pc-ip>:8080/<image> | ubiupdatevol /dev/ubi0_<id> -s <size> -
head -c <size> /dev/ubi0_<id> | md5sum         # must be <md5>
```

The radio fetches the file over its normal network: Ethernet, or WLAN if stock firmware was set up for it. A stock radio has `kernel_bak`, `cramfs_bak`, `ubifs`, `kernel`, `cramfs`. Current images are about 10 MB; if one is larger than 96 LEBs (12,386,304 bytes), create the volume with at least `ceil(size / 129024)` LEBs. RedBoot copies the whole volume to RAM, so a few spare LEBs are harmless.

## 5. Test the flashed copy from RedBoot (writes nothing)

Hold PAUSE while switching on, then:

```
ubi attach -f 0x80000 -l 0x07EC0000 -w 0x800 -e 0x20000 -s 0x200
ubi load -b 0x100000 kernel_new
exec -c "console=ttymxc1,115200 noinitrd init=/linuxrc ubi.mtd=1 root=/dev/mtdblock:cramfs"
```

The `ubi attach` arguments come from the stock boot script. `fconfig -l` lists them without changing anything; never change `fconfig`. RedBoot prints `Using base address 0x00100000 and length 0x00bd0000` (96 LEBs) and the kernel boots to SqueezePlay as in step 3.

## 6. Make it the default

In the root shell of the system booted in step 5:

```sh
ubirename /dev/ubi0 kernel kernel_stock kernel_new kernel
sync
reboot
```

The rename is one atomic UBI volume-table update. From now on, RedBoot autoboots linux-baby with the unchanged stock boot script.

## Going back

- **To stock** from either system (stock also has `ubirename`): `ubirename /dev/ubi0 kernel kernel_baby kernel_stock kernel`, then reboot. Swap the names again to return to linux-baby.
- **If linux-baby does not boot:** hold PAUSE while switching on, run `ubi attach …` as in step 5, then `ubi load -b 0x100000 kernel_stock` and the same `exec -c "…"`. Do the rename above from the running stock system.
- **Later linux-baby updates:**
  1. Repeat steps 4-6 with a new volume name, and rename the running linux-baby volume aside in the same `ubirename` (for example `kernel kernel_old kernel_new kernel`).
  2. Remove the old one with `ubirmvol /dev/ubi0 -N kernel_old` once the new one has booted.
  3. Never write into the volume RedBoot autoboots: an interrupted `ubiupdatevol` leaves it unbootable.

## Managing UBI volumes with ubitui

`ubitui` is a cfdisk-like terminal UI for the UBI volumes (source in `tools/ubitui`). It shows them as a table with a usage bar. It can do what the mtd-utils UBI tools do: create, delete, resize, rename atomically, write images, attach, detach, format, scan, ubiblock. It can also make backups, compare checksums, copy volumes, show a hex view, and show LEB and erase-counter maps. It runs over SSH or on the serial console and needs no terminfo.

The images contain it: the initramfs puts it in a RAM layer under the `/sbin` overlay, so `ubitui` is simply in the `PATH` and nothing is written to the `ubifs` volume. It costs about 240 KiB of RAM. A `ubitui` in the `ubifs` volume's `/sbin` would take precedence.

On a radio running another kernel (stock, or a linux-baby image older than `7.3.0-00016`), copy the binary that `./build.sh` leaves in `work/ubitui/ubitui`, e.g. with `wget -O /root/ubitui http://<pc-ip>:8080/ubitui && chmod +x /root/ubitui`. `/root` is stored in the `ubifs` volume, so this copy survives reboots. Under the stock 2.6.26 kernel it is untested; that kernel predates some of the UBI interfaces it uses (erase-counter map, read check, ubiblock), which then report an error.

Usage:

- Changes happen immediately and destructive ones ask you to type `yes`. Renames are the exception: they are staged and applied together in one atomic `UBI_IOCRNVOL`, like `ubirename`. Renaming a volume to the name of a volume that is not renamed itself removes that volume in the same operation.
- Steps 4 and 6 in ubitui: **New** `kernel_new` with size `96 LEB`; **Write** with source `http://<pc-ip>:8080/<image>`; **Rename** `kernel` to `kernel_stock` and `kernel_new` to `kernel`; **Apply**. The write streams the image (nothing is stored in RAM) and verifies it by reading it back. Compare the MD5 it shows with `<md5>`.
- Write sources: a file, a device such as `/dev/ubi0_3`, an `http://` URL, or `-` for data piped in (`wget -O - … | ubitui`; this needs SSH, the serial console shell has no controlling terminal).
- On the serial console, kernel messages can overwrite the screen; Ctrl-L redraws. Without `stty rows N cols M`, the terminal size is asked from the terminal program.

## Without a serial console (untested)

Steps 4 and 6 also work from the stock firmware over SSH:

1. In SqueezePlay, open Settings → Advanced → SSH and enable it. The screen shows the root password (`1234`) and the IP address.
2. `ssh root@<radio-ip>`.
3. Run the commands of step 4, then `ubirename /dev/ubi0 kernel kernel_stock kernel_new kernel` and `reboot`.

This skips both tests. If the new kernel does not come up, the radio can only be recovered through the serial console ("If linux-baby does not boot" above).

