#!/bin/sh
# Runs only inside the disposable Linux guest, never on the Mac or Docker host.
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /tmp /run /source /opt/rblive4
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /tmp

if [ -f /native-guest ]; then
    modprobe binfmt_misc
    mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
    # ELF32 little-endian ARM; F keeps the static interpreter available inside
    # the firmware chroot, which deliberately contains only its ARM32 libraries.
    printf '%s' ':rx3-arm:M::\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/bin/qemu-arm-static:F' >/proc/sys/fs/binfmt_misc/register || exec sh
    echo 'RBPI4B_MACOS: native ARM64 kernel; ARM32 user emulator registered'
fi

for module in virtio_pci virtio_gpu 9pnet_virtio 9p evdev xhci_pci usbhid hid_generic snd_usb_audio snd_seq snd_seq_midi virtio_console overlay sd_mod usb_storage; do
    modprobe "$module" 2>/dev/null || true
done
# devtmpfs supplies device nodes; Debian's installer BusyBox does not ship mdev.
echo 'RBPI4B_MACOS: Linux guest ready'
mount -t 9p -o trans=virtio,version=9p2000.L,ro source /source || exec sh

if [ ! -f /source/rbpi4b-pi4.tgz ]; then
    echo 'RBPI4B_MACOS: player assets are not assembled yet; diagnostic shell ready'
    cat /proc/fb
    exec sh
fi

tar -xzf /source/rbpi4b-pi4.tgz -C /opt/rblive4 || exec sh
ROOT=/opt/rblive4/rbx3-run
for dir in dev proc sys tmp; do
    mkdir -p "$ROOT/$dir"
    mount --bind "/$dir" "$ROOT/$dir"
done
# DeviceSQL talks to edb_streamd over localhost; no external NIC is attached.
chroot "$ROOT" /sbin/ifconfig lo 127.0.0.1 up
USB_READY=0
mkdir -p /tmp/usb-lower /tmp/usb-upper /tmp/usb-work /tmp/usb1
if mount -t 9p -o trans=virtio,version=9p2000.L,ro usb1 /tmp/usb-lower 2>/dev/null; then
    if [ -f /source/media-id.fat ] && mount -t overlay overlay -o lowerdir=/tmp/usb-lower,upperdir=/tmp/usb-upper,workdir=/tmp/usb-work /tmp/usb1; then
        # DeviceSQL opens the database O_RDWR even for browsing. Its writes go
        # to tmpfs copy-on-write storage and are discarded when the VM stops.
        mkdir -p "$ROOT/media/usb1/sda1"
        mount --bind /tmp/usb1 "$ROOT/media/usb1/sda1"
        USB_READY=1
        cat /source/usb-label >/tmp/udev_usb1.label 2>/dev/null || printf '%s' 'RX3 Mac USB' >/tmp/udev_usb1.label
    else
        echo 'RBPI4B_MACOS: USB overlay unavailable; check guest modules and media identity image'
    fi
fi
if [ -x /bin/midi-guest ] && [ ! -f /native-guest ]; then
    for description in /sys/class/virtio-ports/*/name; do
        if [ "$(cat "$description" 2>/dev/null)" = flx4 ]; then
            port="$(basename "$(dirname "$description")")"
            /bin/midi-guest "/dev/$port" >/tmp/midi-bridge.log 2>&1 &
            break
        fi
    done
fi
mkdir -p "$ROOT/root/settings" "$ROOT/var/run" "$ROOT/root/.directfb"
# The shim consumes evdev itself. DirectFB's keyboard driver also consumes the
# same keys and applies the firmware's PC shortcuts, causing duplicate actions.
for config in "$ROOT/etc/directfbrc" "$ROOT/usr/etc/directfbrc"; do
    printf 'disable-module=keyboard\ndisable-module=linux_input\n' >> "$config"
done
# Poll-only interfaces must be FIFOs. The installer BusyBox lacks mkfifo, so use
# the firmware's BusyBox explicitly; silently missing FIFOs breaks USB events.
for device in subucom_spi1.0 subucom_spi2.0 subucom_spi_rdy3.0 subucom_spi_rdy4.0 hidg0; do
    chroot "$ROOT" /bin/busybox mkfifo "/dev/$device" || exec sh
done
touch /dev/gpiodrv /dev/rtc0 /dev/printkdrv0 /dev/tsc2007_2-0048
mkdir -p /tmp/Settings /tmp/usb1 /tmp/usb2
for fifo in udev_usb1 udev_usb2 udev_usbctn1 udev_usbctn2; do
    chroot "$ROOT" /bin/busybox mkfifo "/tmp/$fifo" || exec sh
done

set -a
. /opt/rblive4/rb.conf
set +a
export RB_FB_DEV=/dev/fb0 RB_DFB_PRESENT=off RB_FB_LIE_BPP=32
export RB_DFB_ROTATE=off RB_AUDIO_DEV=plughw:0,0 RB_AUDIO_MIRROR_DEV= RB_AUDIO_FMT=s16_le
# QEMU USB audio is 48 kHz; plughw resamples the firmware's 44.1 kHz stream.
# Retain the original larger ring as an explicit stable fallback. The smaller
# responsive ring and bounded socket queue reduce producer look-ahead.
audio_periods=4
audio_queue=2
export RB_JOG_IDLE_MS=40
for option in $(cat /proc/cmdline); do
    if [ "$option" = rx3_latency=stable ]; then
        audio_periods=16
        audio_queue=10
        export RB_JOG_IDLE_MS=120
    fi
done
# 4 channels: master on 0,1 and the player's headphone/cue mix on 2,3. QEMU's mixer
# is stereo-only, so the launcher adds a second usb-audio card (USB port 4) for the
# cue mix; the host adapter routes it to the controller's phones (DDJ-SB3) or drops it
# on a stereo device. Falls back to 2 without that card; rx3_audio_channels=2 forces it.
audio_channels=4
for option in $(cat /proc/cmdline); do
    [ "$option" = rx3_audio_channels=2 ] && audio_channels=2
done
export RB_AUDIO_CHANNELS=$audio_channels RB_AUDIO_PERIODS=$((audio_periods * 4))
export RB_LED_DISABLE=1 RB_NETALIAS=0 RB_MIDI_MAP=flx4 RB_EVDEV_MAP=kbd RB_SCHED_RT=0
export RB_POINT_MENU_MOUSE=1 RB_MIDI_DUMP=/tmp/macos-midi.dump
export RB_POINT_CURSOR=1 RB_POINT_CURSOR_ABS=1
export RB_POINT_CURSOR_MS=2
export FB_VSYNC_HZ=60 FB_VSYNC_RATE=1 POINT_MENU_MS=2
export RB_POINT_DEV=
if [ -f /native-guest ]; then
    chroot "$ROOT" /bin/busybox mkfifo /tmp/mac-pointer /tmp/mac-keyboard || exec sh
    /bin/native-input tablet /tmp/mac-pointer >/tmp/native-pointer.log 2>&1 &
    /bin/native-input keyboard /tmp/mac-keyboard >/tmp/native-keyboard.log 2>&1 &
    export RB_POINT_KIND=abs RB_POINT_DEV=/tmp/mac-pointer RB_KBD_RAW_DEV=/tmp/mac-keyboard
    for description in /sys/class/virtio-ports/*/name; do
        if [ "$(cat "$description" 2>/dev/null)" = flx4 ]; then
            export RB_MIDI_RAW_DEV="/dev/$(basename "$(dirname "$description")")"
            export RB_MIDI_RAW_OUTPUT=1 RB_LED_DISABLE=0
        fi
    done
    # Device enumeration is asynchronous on the much faster native boot.
    attempt=0
    while [ ! -e /dev/snd/pcmC0D0p ] && [ "$attempt" -lt 10 ]; do
        sleep 1
        attempt=$((attempt + 1))
    done
    # /proc/asound/cards: " 0 [Audio ]: ..." then "... at usb-0000:00:02.0-3, full speed".
    usb_card() { awk -v port="-$1," '/^ *[0-9]+ \[/ { card = $1 } index($0, port) { print card; exit }' /proc/asound/cards; }
    master_card=$(usb_card 3)
    phones_card=$(usb_card 4)
    if [ "$audio_channels" = 4 ] && [ -z "$phones_card" ]; then
        attempt=0
        while [ -z "$phones_card" ] && [ "$attempt" -lt 5 ]; do
            sleep 1
            attempt=$((attempt + 1))
            master_card=$(usb_card 3)
            phones_card=$(usb_card 4)
        done
    fi
    [ -n "$master_card" ] || master_card=0
    if [ -z "$phones_card" ] || [ "$phones_card" = "$master_card" ]; then
        audio_channels=2
    fi
    echo "RBPI4B_MACOS: audio master card $master_card, phones card ${phones_card:-none}, $audio_channels channel(s)"
    export RB_AUDIO_CHANNELS=$audio_channels
    # This sysctl affects only the disposable guest. Backpressure still clocks
    # the firmware; fewer queued 64-frame packets lower stale audio lead.
    printf '%s\n' "$audio_queue" >/proc/sys/net/unix/max_dgram_qlen
    /bin/native-audio /tmp/native-audio.sock "$audio_periods" "$audio_channels" \
        "plughw:$master_card,0" "plughw:${phones_card:-1},0" >/tmp/native-audio.log 2>&1 &
    attempt=0
    while [ ! -S /tmp/native-audio.sock ] && [ "$attempt" -lt 10 ]; do
        sleep 1
        attempt=$((attempt + 1))
    done
    if [ ! -S /tmp/native-audio.sock ]; then
        echo 'RBPI4B_MACOS: native audio bridge failed'
        cat /tmp/native-audio.log
        exec sh
    fi
    export RB_AUDIO_SOCKET=/tmp/native-audio.sock
fi
. /source/shim-env.sh
export LD_LIBRARY_PATH=/usr/lib:/lib
echo 'RBPI4B_MACOS: framebuffer'
cat /proc/fb
echo 'RBPI4B_MACOS: ALSA devices'
cat /proc/asound/cards

export EDB_BIN=/usr/bin
chroot "$ROOT" /lib/ld-2.13.so /usr/bin/edb_streamd >/tmp/edb_streamd.log 2>&1 &
sleep 1
echo 'RBPI4B_MACOS: launching XDJ-RX3 player'
chroot "$ROOT" /usr/bin/env LD_PRELOAD="$RB_LD_PRELOAD" /lib/ld-2.13.so /root/pdj/rbp -a >/tmp/rbp.log 2>&1 &
PLAYER_PID=$!
sleep 8
tail -35 /tmp/rbp.log
if kill -0 "$PLAYER_PID" 2>/dev/null; then
    echo "RBPI4B_MACOS: player running (pid $PLAYER_PID); logs /tmp/rbp.log and /tmp/knobshim.log"
else
    echo 'RBPI4B_MACOS: player exited; inspect /tmp/rbp.log'
fi
# The optional export is USB1; the original host share stays read-only.
if [ "$USB_READY" = 1 ]; then
    if [ -f /tmp/usb1/PIONEER/rekordbox/export.pdb ]; then
        # DeviceSQL starts asynchronously and can miss its first mount event.
        (
            attempt=0
            while [ "$attempt" -lt 12 ] && kill -0 "$PLAYER_PID" 2>/dev/null; do
                timeout 3 sh -c 'printf "%s" "umount /media/usb1/sda1" > /tmp/udev_usb1'
                sleep 1
                timeout 3 sh -c 'printf "%s" "mount /media/usb1/sda1" > /tmp/udev_usb1'
                sleep 5
                if ls -l "/proc/$PLAYER_PID/fd" 2>/dev/null | grep -Fq '/media/usb1/sda1/PIONEER/rekordbox/export.pdb'; then
                    echo 'RBPI4B_MACOS: player opened read-only USB database'
                    exit 0
                fi
                attempt=$((attempt + 1))
            done
            echo 'RBPI4B_MACOS: USB database not opened; check DeviceSQL logs'
        ) &
    else
        echo 'RBPI4B_MACOS: USB has no PIONEER/rekordbox/export.pdb'
    fi
fi
exec sh
