#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

qemu-system-x86_64 -m 2G -enable-kvm -cdrom "$ROOT_DIR/build/mljOS.iso"
