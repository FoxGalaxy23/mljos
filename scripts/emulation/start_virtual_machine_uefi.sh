#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

KVM_OPTS=""
[ -w /dev/kvm ] && KVM_OPTS="-enable-kvm"

DISK_ARGS=()
USB_ARGS=()
ISO_ARGS=()
DRIVES=(a b c d)
INDEX=0

for disk in "$ROOT_DIR"/build/disk*.qcow2; do
    [ -e "$disk" ] || continue
    [ "$INDEX" -lt "${#DRIVES[@]}" ] || break
    DISK_ARGS+=("-hd${DRIVES[$INDEX]}" "$disk")
    INDEX=$((INDEX + 1))
done

if [ -f "$ROOT_DIR/build/usb.img" ]; then
    USB_ARGS+=("-device" "piix3-usb-uhci,id=uhci")
    USB_ARGS+=("-drive" "if=none,id=usbstick,format=raw,file=$ROOT_DIR/build/usb.img")
    USB_ARGS+=("-device" "usb-storage,bus=uhci.0,drive=usbstick")
fi

BOOT_ORDER="c"
if [ -f "$ROOT_DIR/build/mljOS.iso" ]; then
    ISO_ARGS+=("-cdrom" "$ROOT_DIR/build/mljOS.iso")
    BOOT_ORDER="d"
fi

OVMF_CODE=""
OVMF_VARS_TEMPLATE=""
for code_path in \
    "/usr/share/OVMF/OVMF_CODE.fd" \
    "/usr/share/edk2/ovmf/OVMF_CODE.fd" \
    "/usr/share/edk2/x64/OVMF_CODE.fd"; do
    if [ -f "$code_path" ]; then
        OVMF_CODE="$code_path"
        break
    fi
done

for vars_path in \
    "/usr/share/OVMF/OVMF_VARS.fd" \
    "/usr/share/edk2/ovmf/OVMF_VARS.fd" \
    "/usr/share/edk2/x64/OVMF_VARS.fd"; do
    if [ -f "$vars_path" ]; then
        OVMF_VARS_TEMPLATE="$vars_path"
        break
    fi
done

if [ -z "$OVMF_CODE" ] || [ -z "$OVMF_VARS_TEMPLATE" ]; then
    echo "OVMF firmware not found."
    echo "Install package: ovmf"
    exit 1
fi

OVMF_VARS_RUNTIME="$ROOT_DIR/build/OVMF_VARS.fd"
cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS_RUNTIME"

qemu-system-x86_64 $KVM_OPTS -m 4G \
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,format=raw,file=$OVMF_VARS_RUNTIME" \
    "${DISK_ARGS[@]}" "${USB_ARGS[@]}" "${ISO_ARGS[@]}" \
    -boot "order=$BOOT_ORDER,menu=on"
