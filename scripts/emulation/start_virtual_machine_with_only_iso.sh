#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

KVM_OPTS=""
[ -w /dev/kvm ] && KVM_OPTS="-enable-kvm"

qemu-system-x86_64 $KVM_OPTS -m 2G -cdrom "$ROOT_DIR/build/mljOS.iso"