#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

qemu-system-x86_64 -m 4G -enable-kvm -hda "$ROOT_DIR/build/disk.qcow2" -cdrom "$ROOT_DIR/build/mljOS.iso" -boot d
