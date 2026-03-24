#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Проверка поддержки KVM
ACCEL=""
if [ -e /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL="-enable-kvm"
fi

DISK_ARGS=()
DRIVES=(a b c d)
INDEX=0

for disk in "$ROOT_DIR"/build/disk*.qcow2; do
    [ -e "$disk" ] || continue
    [ "$INDEX" -lt "${#DRIVES[@]}" ] || break
    DISK_ARGS+=("-hd${DRIVES[$INDEX]}" "$disk")
    INDEX=$((INDEX + 1))
done

# Используем переменную $ACCEL
qemu-system-x86_64 $ACCEL -m 4G "${DISK_ARGS[@]}"