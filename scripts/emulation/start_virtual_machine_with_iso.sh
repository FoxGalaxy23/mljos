#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

KVM_OPTS=""
[ -w /dev/kvm ] && KVM_OPTS="-enable-kvm"

DISK_ARGS=()
DRIVES=(a b c d)
INDEX=0

for disk in "$ROOT_DIR"/build/disk*.qcow2; do
    [ -e "$disk" ] || continue
    [ "$INDEX" -lt "${#DRIVES[@]}" ] || break
    DISK_ARGS+=("-hd${DRIVES[$INDEX]}" "$disk")
    INDEX=$((INDEX + 1))
done

qemu-system-x86_64 $KVM_OPTS -m 4G "${DISK_ARGS[@]}" -cdrom "$ROOT_DIR/build/mljOS.iso" -boot d